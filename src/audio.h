// audio.h - live audio engine: miniaudio duplex device (mic -> chain -> output) plus an optional
// monitor playback device fed through a lock-free ring buffer.
//
// Threading: every public method is called from the UI thread. The audio callbacks only read
// the latest control block (triple buffer), call voice::Chain::process() and update atomics:
// no allocation, no lock, no I/O.
#pragma once

#include "voice_chain.h"

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace audio {

constexpr unsigned kSampleRate = 48000;
constexpr unsigned kPeriodFrames = 256;

// Single-producer / single-consumer "latest value" triple buffer.
template <class T>
class Latest {
public:
	void write(const T &v) {
		buf_[w_] = v;
		w_ = back_.exchange(w_ | kNew, std::memory_order_acq_rel) & 3;
	}
	// Returns true when a new value was picked up.
	bool read(T &out) {
		if (!(back_.load(std::memory_order_acquire) & kNew)) return false;
		r_ = back_.exchange(r_, std::memory_order_acq_rel) & 3;
		out = buf_[r_];
		return true;
	}

private:
	static constexpr int kNew = 4;
	T buf_[3]{};
	int w_ = 0, r_ = 1;
	std::atomic<int> back_{2};
};

struct Control {
	voice::Params params;
	bool bypass = false;
	float inGain = 1, outGain = 1, monitorGain = 0.8f;  // linear
};

struct DeviceInfo {
	std::string name;
	bool isDefault = false;
};

struct Stats {
	double cpuAvg = 0, cpuPeak = 0;  // processing time / period, over the last second (0..1+)
	unsigned glitches = 0, late = 0;
	unsigned monitorUnderruns = 0;
	float inPeak = 0, outPeak = 0;   // linear peak since the previous poll
};

struct StreamInfo {
	std::string inName, outName, monitorName;
	unsigned inRate = 0, outRate = 0, monitorRate = 0;
	double engineMs = 0, inMs = 0, outMs = 0, bufferMs = 0, totalMs = 0, minMs = 0;
	bool resampled() const { return inRate != kSampleRate || outRate != kSampleRate; }
};

class Engine {
public:
	Engine();
	~Engine();

	bool init(std::string &err);  // audio context (does not open any device)
	void refreshDevices();
	const std::vector<DeviceInfo> &inputs() const { return inputs_; }
	const std::vector<DeviceInfo> &outputs() const { return outputs_; }
	int findCableOutput() const;  // first playback device containing "CABLE Input", or -1

	// Device names; empty = system default. Empty monitor = no monitor. Restarts if running.
	bool start(const std::string &in, const std::string &out, const std::string &monitor, std::string &err);
	void stop();
	bool running() const { return running_; }
	bool monitorRunning() const { return monitorOpen_; }
	const StreamInfo &info() const { return info_; }

	void setControl(const Control &c) { ctl_.write(c); }
	void setInitialControl(const Control &c) { initial_ = c; }

	// Call ~every frame; CPU figures are refreshed once per second, peaks are reset on each call.
	Stats poll();

private:
	struct Impl;
	std::unique_ptr<Impl> m_;
	std::vector<DeviceInfo> inputs_, outputs_;
	StreamInfo info_;
	bool running_ = false, monitorOpen_ = false;
	Latest<Control> ctl_;
	Control initial_;
	Stats last_;
	double lastCpuT_ = 0;

	friend struct Callbacks;
};

}  // namespace audio
