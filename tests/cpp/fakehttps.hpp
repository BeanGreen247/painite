// A local HTTPS server with a throwaway self-signed certificate, used as a fake Mojang session / Xbox authorization service.
#pragma once
#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <utility>

namespace pt {

class FakeHttps {
public:
    // handler(path) -> (status, body); status 204 sends no body
    using Handler = std::function<std::pair<int, std::string>(const std::string& path)>;
    FakeHttps(const std::string& dir, Handler h);
    ~FakeHttps();
    int port = 0;
    std::string cert_path;   // PEM of the certificate (the server under test trusts it as its CA)
private:
    void run();
    Handler handler_;
    int lfd_ = -1;
    void* ctx_ = nullptr;
    std::atomic<bool> stop_{false};
    std::thread th_;
};

}  // namespace pt
