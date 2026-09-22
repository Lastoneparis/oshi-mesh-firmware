#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Request/response encoding for the OSHI gateway HTTPS API (/v2/mesh/*), kept free of Arduino types.
namespace oshi
{

std::string base64Encode(const uint8_t *data, size_t len);
bool base64Decode(const std::string &in, std::vector<uint8_t> &out);

std::string uplinkBody(uint32_t gateway, const uint8_t *frame, size_t len);
std::string pullBody(uint32_t gateway, const uint8_t *pull, size_t len);

struct DownlinkFrame {
    uint32_t seq = 0;
    std::vector<uint8_t> bytes;
};

// Parses {"nodeNum":N,"frames":[{"seq":S,"frame":"b64"},...],...}. Frames that fail to decode are skipped.
bool parsePullResponse(const std::string &json, uint32_t &nodeNum, std::vector<DownlinkFrame> &frames);

// A 'D' downlink body (see mesh_gateway.js) carries the relay sequence number at offset 3.
bool downlinkSeq(const uint8_t *body, size_t len, uint32_t &seq);

} // namespace oshi
