#include "OshiGatewayCodec.h"
#include <cstdlib>
#include <cstring>

namespace oshi
{

namespace
{
const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int b64Value(char c)
{
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;
    if (c >= '0' && c <= '9')
        return c - '0' + 52;
    if (c == '+' || c == '-')
        return 62;
    if (c == '/' || c == '_')
        return 63;
    return -1;
}

bool readUint(const std::string &s, size_t pos, uint32_t &out)
{
    if (pos >= s.size() || s[pos] < '0' || s[pos] > '9')
        return false;
    unsigned long long v = 0;
    while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') {
        v = v * 10 + (s[pos] - '0');
        if (v > 0xFFFFFFFFULL)
            return false;
        pos++;
    }
    out = uint32_t(v);
    return true;
}
} // namespace

std::string base64Encode(const uint8_t *data, size_t len)
{
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = uint32_t(data[i]) << 16;
        if (i + 1 < len)
            v |= uint32_t(data[i + 1]) << 8;
        if (i + 2 < len)
            v |= data[i + 2];
        out += B64[(v >> 18) & 63];
        out += B64[(v >> 12) & 63];
        out += i + 1 < len ? B64[(v >> 6) & 63] : '=';
        out += i + 2 < len ? B64[v & 63] : '=';
    }
    return out;
}

bool base64Decode(const std::string &in, std::vector<uint8_t> &out)
{
    out.clear();
    uint32_t acc = 0;
    int bits = 0;
    for (char c : in) {
        if (c == '=')
            break;
        int v = b64Value(c);
        if (v < 0)
            return false;
        acc = (acc << 6) | uint32_t(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(uint8_t((acc >> bits) & 0xFF));
        }
    }
    return true;
}

std::string uplinkBody(uint32_t gateway, const uint8_t *frame, size_t len)
{
    return "{\"gateway\":" + std::to_string(gateway) + ",\"frame\":\"" + base64Encode(frame, len) + "\"}";
}

std::string pullBody(uint32_t gateway, const uint8_t *pull, size_t len)
{
    return "{\"gateway\":" + std::to_string(gateway) + ",\"pull\":\"" + base64Encode(pull, len) + "\"}";
}

bool parsePullResponse(const std::string &json, uint32_t &nodeNum, std::vector<DownlinkFrame> &frames)
{
    frames.clear();
    size_t p = json.find("\"nodeNum\":");
    if (p == std::string::npos || !readUint(json, p + 10, nodeNum))
        return false;
    size_t pos = json.find("\"frames\":[");
    if (pos == std::string::npos)
        return false;
    for (;;) {
        size_t s = json.find("\"seq\":", pos);
        if (s == std::string::npos)
            break;
        DownlinkFrame f;
        if (!readUint(json, s + 6, f.seq))
            break;
        size_t fr = json.find("\"frame\":\"", s);
        if (fr == std::string::npos)
            break;
        size_t start = fr + 9, end = json.find('"', start);
        if (end == std::string::npos)
            break;
        if (base64Decode(json.substr(start, end - start), f.bytes) && !f.bytes.empty())
            frames.push_back(std::move(f));
        pos = end + 1;
    }
    return true;
}

bool downlinkSeq(const uint8_t *body, size_t len, uint32_t &seq)
{
    if (len < 7 || body[0] != 0x44 || body[1] != 1)
        return false;
    seq = uint32_t(body[3]) | (uint32_t(body[4]) << 8) | (uint32_t(body[5]) << 16) | (uint32_t(body[6]) << 24);
    return true;
}

} // namespace oshi
