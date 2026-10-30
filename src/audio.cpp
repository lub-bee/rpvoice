// audio.cpp - see audio.h. Device setup and latency estimate follow wintest/main.cpp.
#include "audio.h"

#include "miniaudio/miniaudio.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace audio {

namespace {
using Clock = std::chrono::steady_clock;

double nowSec() { return std::chrono::duration<double>(Clock::now().time_since_epoch()).count(); }

bool containsCI(const std::string &hay, const char *needle) {
	std::string h(hay), n(needle);
	for (auto &c : h) c = char(std::tolower((unsigned char)c));
	for (auto &c : n) c = char(std::tolower((unsigned char)c));
	return h.find(n) != std::string::npos;
}

void atomicMax(std::atomic<unsigned> &a, unsigned v) {
	unsigned cur = a.load(std::memory_order_relaxed);
	while (v > cur && !a.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {}
}
void atomicMaxF(std::atomic<float> &a, float v) {
	float cur = a.load(std::memory_order_relaxed);
	while (v > cur && !a.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {}
}

// Linear gain ramp from g0 to g1 over n samples.
inline void ramp(float *v, int n, float g0, float g1) {
	if (g0 == 1 && g1 == 1) return;
	float d = n > 0 ? (g1 - g0) / float(n) : 0;
	for (int i = 0; i < n; ++i) v[i] *= g0 + d * float(i);
}
}  // namespace

struct Engine::Impl {
	ma_context ctx{};
	bool ctxOk = false;
	ma_device_info *playInfos = nullptr, *capInfos = nullptr;
	ma_uint32 playCount = 0, capCount = 0;

	ma_device duplex{}, monitor{};
	bool duplexOk = false, monitorOk = false;
	ma_pcm_rb rb{};
	bool rbOk = false;

	std::unique_ptr<voice::Chain> chain;
	Latest<Control> *ctl = nullptr;
	Control cur;          // audio-thread copy
	float inG = 1, outG = 1, monG = 0;
	static constexpr int kTmp = 4096;
	float tmp[kTmp];

	// Stats (audio thread writes, UI reads/resets).
	std::atomic<unsigned long long> busyNs{0}, periodNs{0};
	std::atomic<unsigned> peakPermille{0}, glitches{0}, late{0}, monUnderruns{0};
	std::atomic<float> inPeak{0}, outPeak{0};
	Clock::time_point lastCb{};
	bool haveLast = false;
	ma_uint32 monTargetFill = 0;
	bool monPrimed = false;
};

struct Callbacks {
	static void duplex(ma_device *dev, void *output, const void *input, ma_uint32 frames) {
		auto *m = static_cast<Engine::Impl *>(dev->pUserData);
		auto t0 = Clock::now();
		double periodSec = double(frames) / kSampleRate;
		if (m->haveLast) {
			double gap = std::chrono::duration<double>(t0 - m->lastCb).count();
			// Shared-mode WASAPI may deliver bursts, so only count clearly abnormal gaps (as wintest).
			if (gap > 2.5 * std::max(periodSec, double(kPeriodFrames) / kSampleRate) + 0.002) m->glitches++;
		}
		m->lastCb = t0;
		m->haveLast = true;

		Control c;
		if (m->ctl->read(c)) {
			m->cur = c;
			m->chain->setParams(c.params, c.bypass);
		}
		auto *out = static_cast<float *>(output);
		const auto *in = static_cast<const float *>(input);
		float inPk = 0, outPk = 0;
		for (ma_uint32 done = 0; done < frames;) {
			int n = int(std::min<ma_uint32>(frames - done, Engine::Impl::kTmp));
			if (in) std::memcpy(m->tmp, in + done, size_t(n) * sizeof(float));
			else std::memset(m->tmp, 0, size_t(n) * sizeof(float));
			float g1 = m->cur.inGain;
			ramp(m->tmp, n, m->inG, g1);
			m->inG = g1;
			for (int i = 0; i < n; ++i) inPk = std::max(inPk, std::fabs(m->tmp[i]));
			m->chain->process(m->tmp, out + done, n);
			g1 = m->cur.outGain;
			ramp(out + done, n, m->outG, g1);
			m->outG = g1;
			for (int i = 0; i < n; ++i) {
				out[done + i] = std::clamp(out[done + i], -1.f, 1.f);
				outPk = std::max(outPk, std::fabs(out[done + i]));
			}
			if (m->rbOk) {  // monitor feed; drop when the ring is full (monitor callback stalled)
				float mg = m->cur.monitorGain;
				ma_uint32 left = ma_uint32(n), off = 0;
				float g0 = m->monG, dg = (mg - g0) / float(n);
				while (left > 0) {
					ma_uint32 cnt = left;
					void *p = nullptr;
					if (ma_pcm_rb_acquire_write(&m->rb, &cnt, &p) != MA_SUCCESS || cnt == 0) break;
					float *dst = static_cast<float *>(p);
					for (ma_uint32 i = 0; i < cnt; ++i) dst[i] = out[done + off + i] * (g0 + dg * float(off + i));
					ma_pcm_rb_commit_write(&m->rb, cnt);
					left -= cnt;
					off += cnt;
				}
				m->monG = mg;
			}
			done += ma_uint32(n);
		}
		atomicMaxF(m->inPeak, inPk);
		atomicMaxF(m->outPeak, outPk);

		auto used = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count();
		auto period = (long long)(periodSec * 1e9);
		m->busyNs += (unsigned long long)used;
		m->periodNs += (unsigned long long)period;
		if (used > period) m->late++;
		atomicMax(m->peakPermille, unsigned(period > 0 ? 1000.0 * double(used) / double(period) : 0));
	}

	static void monitor(ma_device *dev, void *output, const void *, ma_uint32 frames) {
		auto *m = static_cast<Engine::Impl *>(dev->pUserData);
		float *out = static_cast<float *>(output);
		ma_uint32 avail = ma_pcm_rb_available_read(&m->rb);
		// Wait for a small cushion before starting, and drop excess so the two device clocks
		// cannot let the monitor delay grow without bound.
		if (!m->monPrimed) {
			if (avail < m->monTargetFill) { std::memset(out, 0, frames * sizeof(float)); return; }
			m->monPrimed = true;
		}
		if (avail > 3 * m->monTargetFill + frames) {
			ma_pcm_rb_seek_read(&m->rb, avail - m->monTargetFill - frames);
		}
		ma_uint32 done = 0;
		while (done < frames) {
			ma_uint32 cnt = frames - done;
			void *p = nullptr;
			if (ma_pcm_rb_acquire_read(&m->rb, &cnt, &p) != MA_SUCCESS || cnt == 0) break;
			std::memcpy(out + done, p, cnt * sizeof(float));
			ma_pcm_rb_commit_read(&m->rb, cnt);
			done += cnt;
		}
		if (done < frames) {
			std::memset(out + done, 0, (frames - done) * sizeof(float));
			m->monUnderruns++;
			m->monPrimed = false;
		}
	}
};

Engine::Engine() : m_(new Impl) {}

Engine::~Engine() {
	stop();
	if (m_->ctxOk) ma_context_uninit(&m_->ctx);
}

bool Engine::init(std::string &err) {
#ifdef _WIN32
	ma_backend backends[] = {ma_backend_wasapi};
	ma_result r = ma_context_init(backends, 1, nullptr, &m_->ctx);
#else
	ma_result r = ma_context_init(nullptr, 0, nullptr, &m_->ctx);
#endif
	if (r != MA_SUCCESS) {
		err = "impossible d'initialiser l'audio";
		return false;
	}
	m_->ctxOk = true;
	refreshDevices();
	return true;
}

void Engine::refreshDevices() {
	inputs_.clear();
	outputs_.clear();
	if (!m_->ctxOk) return;
	if (ma_context_get_devices(&m_->ctx, &m_->playInfos, &m_->playCount, &m_->capInfos, &m_->capCount) != MA_SUCCESS) {
		m_->playCount = m_->capCount = 0;
		return;
	}
	for (ma_uint32 i = 0; i < m_->capCount; ++i) inputs_.push_back({m_->capInfos[i].name, m_->capInfos[i].isDefault != 0});
	for (ma_uint32 i = 0; i < m_->playCount; ++i) outputs_.push_back({m_->playInfos[i].name, m_->playInfos[i].isDefault != 0});
}

int Engine::findCableOutput() const {
	for (size_t i = 0; i < outputs_.size(); ++i)
		if (containsCI(outputs_[i].name, "CABLE Input")) return int(i);
	return -1;
}

bool Engine::start(const std::string &in, const std::string &out, const std::string &mon, std::string &err) {
	stop();
	if (!m_->ctxOk) { err = "audio non initialisé"; return false; }
	Impl &m = *m_;
	auto findId = [](ma_device_info *infos, ma_uint32 count, const std::string &name) -> ma_device_id * {
		if (name.empty()) return nullptr;
		for (ma_uint32 i = 0; i < count; ++i)
			if (name == infos[i].name) return &infos[i].id;
		return nullptr;  // not found: system default
	};

	if (!m.chain) m.chain.reset(new voice::Chain());
	m.chain->prepare(kSampleRate);  // allocates here, never in the callback
	m.cur = initial_;
	ctl_.read(m.cur);  // newest pending value, if any
	m.chain->setParams(m.cur.params, m.cur.bypass, true);
	m.inG = m.cur.inGain;
	m.outG = m.cur.outGain;
	m.monG = m.cur.monitorGain;
	m.ctl = &ctl_;
	m.haveLast = false;
	m.glitches = 0;
	m.late = 0;
	m.monUnderruns = 0;

	// Monitor ring buffer first (the duplex callback writes into it as soon as it starts).
	ma_device_id *monId = mon.empty() ? nullptr : findId(m.playInfos, m.playCount, mon);
	bool wantMonitor = !mon.empty();
	if (wantMonitor) {
		m.rbOk = ma_pcm_rb_init(ma_format_f32, 1, kSampleRate / 4, nullptr, nullptr, &m.rb) == MA_SUCCESS;  // 250 ms
		m.monTargetFill = 2 * kPeriodFrames;
		m.monPrimed = false;
	}

	ma_device_config cfg = ma_device_config_init(ma_device_type_duplex);
	cfg.sampleRate = kSampleRate;
	cfg.periodSizeInFrames = kPeriodFrames;
	cfg.performanceProfile = ma_performance_profile_low_latency;
	cfg.capture.pDeviceID = findId(m.capInfos, m.capCount, in);
	cfg.capture.format = ma_format_f32;
	cfg.capture.channels = 1;
	cfg.capture.shareMode = ma_share_mode_shared;
	cfg.playback.pDeviceID = findId(m.playInfos, m.playCount, out);
	cfg.playback.format = ma_format_f32;
	cfg.playback.channels = 1;
	cfg.playback.shareMode = ma_share_mode_shared;
	cfg.dataCallback = Callbacks::duplex;
	cfg.pUserData = &m;
	cfg.noPreSilencedOutputBuffer = MA_TRUE;  // we always write every output frame
	cfg.wasapi.noAutoConvertSRC = MA_FALSE;
	if (ma_device_init(&m.ctx, &cfg, &m.duplex) != MA_SUCCESS) {
		err = "impossible d'ouvrir le micro / la sortie";
		stop();
		return false;
	}
	m.duplexOk = true;

	if (m.rbOk) {
		ma_device_config mc = ma_device_config_init(ma_device_type_playback);
		mc.sampleRate = kSampleRate;
		mc.periodSizeInFrames = kPeriodFrames;
		mc.performanceProfile = ma_performance_profile_low_latency;
		mc.playback.pDeviceID = monId;
		mc.playback.format = ma_format_f32;
		mc.playback.channels = 1;  // miniaudio duplicates mono to every speaker channel
		mc.playback.shareMode = ma_share_mode_shared;
		mc.dataCallback = Callbacks::monitor;
		mc.pUserData = &m;
		m.monitorOk = ma_device_init(&m.ctx, &mc, &m.monitor) == MA_SUCCESS;
		if (!m.monitorOk) err = "retour casque : impossible d'ouvrir le périphérique";
	}

	// Latency estimate, same formula as wintest.
	auto devMs = [](ma_uint32 period, ma_uint32 periods, ma_uint32 rate) {
		return rate ? 1000.0 * period * periods / rate : 0.0;
	};
	ma_device &d = m.duplex;
	info_ = StreamInfo{};
	info_.inName = d.capture.name;
	info_.outName = d.playback.name;
	info_.inRate = d.capture.internalSampleRate;
	info_.outRate = d.playback.internalSampleRate;
	info_.engineMs = m.chain->latencySamples() * 1000.0 / kSampleRate;
	info_.inMs = devMs(d.capture.internalPeriodSizeInFrames, d.capture.internalPeriods, d.capture.internalSampleRate);
	info_.outMs = devMs(d.playback.internalPeriodSizeInFrames, d.playback.internalPeriods, d.playback.internalSampleRate);
	info_.bufferMs = 1000.0 * kPeriodFrames / kSampleRate;  // duplex ring buffer, ~one period
	info_.totalMs = info_.engineMs + info_.inMs + info_.outMs + info_.bufferMs;
	info_.minMs = info_.engineMs + devMs(d.capture.internalPeriodSizeInFrames, 1, d.capture.internalSampleRate) +
	              devMs(d.playback.internalPeriodSizeInFrames, 1, d.playback.internalSampleRate) + info_.bufferMs;
	if (m.monitorOk) {
		info_.monitorName = m.monitor.playback.name;
		info_.monitorRate = m.monitor.playback.internalSampleRate;
	}

	if (ma_device_start(&m.duplex) != MA_SUCCESS) {
		err = "impossible de démarrer l'audio";
		stop();
		return false;
	}
	running_ = true;
	if (m.monitorOk && ma_device_start(&m.monitor) == MA_SUCCESS) monitorOpen_ = true;
	lastCpuT_ = nowSec();
	m.busyNs = 0;
	m.periodNs = 0;
	m.peakPermille = 0;
	return true;
}

void Engine::stop() {
	Impl &m = *m_;
	if (m.monitorOk) { ma_device_uninit(&m.monitor); m.monitorOk = false; }
	if (m.duplexOk) { ma_device_uninit(&m.duplex); m.duplexOk = false; }
	if (m.rbOk) { ma_pcm_rb_uninit(&m.rb); m.rbOk = false; }
	running_ = false;
	monitorOpen_ = false;
}

Stats Engine::poll() {
	Impl &m = *m_;
	Stats s = last_;
	s.glitches = m.glitches.load();
	s.late = m.late.load();
	s.monitorUnderruns = m.monUnderruns.load();
	s.inPeak = m.inPeak.exchange(0);
	s.outPeak = m.outPeak.exchange(0);
	double t = nowSec();
	if (!running_) {
		s.cpuAvg = s.cpuPeak = 0;
	} else if (t - lastCpuT_ >= 1.0) {
		unsigned long long busy = m.busyNs.exchange(0), per = m.periodNs.exchange(0);
		s.cpuAvg = per ? double(busy) / double(per) : 0;
		s.cpuPeak = m.peakPermille.exchange(0) / 1000.0;
		lastCpuT_ = t;
	}
	last_ = s;
	return s;
}

}  // namespace audio
