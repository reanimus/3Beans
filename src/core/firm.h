#pragma once

#include <cstdint>
#include <string>
#include <vector>

class Core;

// An immutable, validated snapshot: resetting rereads the host file before the
// old core is destroyed. Construction performs no emulated memory or I/O writes.
class FirmImage {
public:
    explicit FirmImage(const std::string &path);
    const std::string path;
    void boot(Core &core) const;

private:
    struct Section { uint32_t offset, address, size; };
    std::vector<uint8_t> data;
    std::vector<Section> sections;
    uint32_t arm9Entry, arm11Entry;
    bool screenInit;
};
