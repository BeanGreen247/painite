// UDP relay that drops and delays datagrams, to exercise RakNet resends on a bad link.
#pragma once
#include <atomic>
#include <thread>

namespace pt {

class LossyProxy {
public:
    LossyProxy(int listen_port, int target_port, double loss_percent, int delay_ms);   // listen_port 0 picks a free one
    ~LossyProxy();
    int port = 0;
    int dropped() const { return dropped_; }
    int forwarded() const { return forwarded_; }
private:
    void run();
    int front_ = -1, back_ = -1, target_;
    double loss_;
    int delay_ms_;
    std::atomic<bool> stop_{false};
    std::atomic<int> dropped_{0}, forwarded_{0};
    std::thread th_;
};

}  // namespace pt
