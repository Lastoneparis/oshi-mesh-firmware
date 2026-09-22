#include "configuration.h"

#if defined(ARCH_ESP32) && HAS_WIFI && !MESHTASTIC_EXCLUDE_WIFI && !MESHTASTIC_EXCLUDE_OSHI

#include "OshiGatewayCodec.h"
#include "OshiGatewayEsp32.h"
#include "OshiGatewayRoots.h"
#include "concurrency/LockGuard.h"
#include "modules/OshiModule.h"
#include "platform/esp32/architecture.h"
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#ifndef OSHI_GATEWAY_URL
#define OSHI_GATEWAY_URL "https://oshi-messenger.com/v2/mesh"
#endif

namespace oshi
{

namespace
{
constexpr size_t MAX_JOBS = 8;
constexpr uint32_t HTTP_TIMEOUT_MS = 10000;
constexpr uint32_t BACKOFF_MS = 30000;
constexpr uint32_t STACK_BYTES = 8192;
} // namespace

Esp32Gateway::Esp32Gateway(uint32_t selfNode) : self(selfNode)
{
    xTaskCreatePinnedToCore(taskEntry, "oshi-gw", STACK_BYTES, this, 1, nullptr, 0);
}

bool Esp32Gateway::online() const
{
    return WiFi.status() == WL_CONNECTED && !backingOff;
}

bool Esp32Gateway::push(Job &&j)
{
    concurrency::LockGuard g(&lock);
    if (jobs.size() >= MAX_JOBS)
        return false;
    jobs.push_back(std::move(j));
    return true;
}

bool Esp32Gateway::uplink(const Message &msg)
{
    Job j;
    j.kind = Job::UPLINK;
    j.bytes = msg.body;
    return push(std::move(j));
}

bool Esp32Gateway::forwardPull(uint32_t fromNode, const uint8_t *pull, size_t len)
{
    Job j;
    j.kind = Job::PULL;
    j.node = fromNode;
    j.bytes.assign(pull, pull + len);
    return push(std::move(j));
}

void Esp32Gateway::loop(uint32_t nowMs)
{
    (void)nowMs;
    std::vector<Result> ready;
    {
        concurrency::LockGuard g(&lock);
        ready.swap(results);
    }
    for (auto &r : ready) {
        Message m;
        m.msgId = r.seq;
        m.dest = r.node;
        m.body = std::move(r.bytes);
        if (oshiModule)
            oshiModule->submitDownlink(m);
    }
}

void Esp32Gateway::taskEntry(void *arg)
{
    static_cast<Esp32Gateway *>(arg)->taskLoop();
}

int Esp32Gateway::post(const char *route, const std::string &body, std::string &response)
{
    WiFiClientSecure tls;
    tls.setCACert(OSHI_GATEWAY_ROOT_CAS);
    tls.setTimeout(HTTP_TIMEOUT_MS / 1000);
    HTTPClient http;
    std::string url = std::string(OSHI_GATEWAY_URL) + route;
    if (!http.begin(tls, url.c_str()))
        return -1;
    http.setTimeout(HTTP_TIMEOUT_MS);
    http.addHeader("Content-Type", "application/json");
    int code = http.POST((uint8_t *)body.data(), body.size());
    if (code > 0)
        response = http.getString().c_str();
    http.end();
    return code;
}

void Esp32Gateway::taskLoop()
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(backingOff ? BACKOFF_MS : 250));
        backingOff = false;
        if (WiFi.status() != WL_CONNECTED)
            continue;
        Job j;
        {
            concurrency::LockGuard g(&lock);
            if (jobs.empty())
                continue;
            j = std::move(jobs.front());
            jobs.pop_front();
        }
        std::string resp;
        bool isPull = j.kind == Job::PULL;
        int code = isPull ? post("/pull", pullBody(self, j.bytes.data(), j.bytes.size()), resp)
                          : post("/uplink", uplinkBody(self, j.bytes.data(), j.bytes.size()), resp);
        if (code < 0 || code == 429 || code >= 500) {
            // Transient: keep the job and slow down. A 4xx is the server rejecting the frame itself, so it is dropped.
            LOG_WARN("OSHI gateway: %s failed (%d), retrying", isPull ? "pull" : "uplink", code);
            concurrency::LockGuard g(&lock);
            if (!isPull)
                jobs.push_front(std::move(j));
            backingOff = true;
            continue;
        }
        if (code != 200) {
            LOG_INFO("OSHI gateway: %s refused (%d)", isPull ? "pull" : "uplink", code);
            continue;
        }
        if (!isPull)
            continue;
        uint32_t node = 0;
        std::vector<DownlinkFrame> frames;
        if (!parsePullResponse(resp, node, frames) || node != j.node)
            continue;
        concurrency::LockGuard g(&lock);
        for (auto &f : frames)
            results.push_back({node, f.seq, std::move(f.bytes)});
    }
}

} // namespace oshi

#endif
