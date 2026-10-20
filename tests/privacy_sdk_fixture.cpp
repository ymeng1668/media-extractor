// Deterministic SDK stand-in for combined-filter integration tests only.
#include <cstdint>

struct ImageSize { int width, height; char reserved[56]; };
struct Memory { uint64_t pts; unsigned char* data; int size; char reserved[48]; };
struct Output { const uint8_t* data; int size; };

extern "C" int RMAIInfo_Parse_Create(ImageSize) { return 1; }
extern "C" int RMAIInfo_Parse_Destroy(int) { return 0; }
extern "C" int RMAIInfo_ParseStruct(int, Memory input, Output* output)
{
    static uint8_t bytes[92]{};
    auto put = [](int offset, uint32_t value) {
        for (int i = 0; i < 4; ++i) bytes[offset + i] = uint8_t(value >> (8 * i));
    };
    put(0, 0x524d4149); put(4, 1 | (1u << 16)); put(8, 92); put(12, 20);
    put(20, uint32_t(input.pts)); put(24, uint32_t(input.pts >> 32));
    const uint32_t algorithm = input.size > 0 && input.data[0] == '!' ? 6 : 2;
    put(28, 2); put(32, algorithm); put(36, 1); put(40, 48); put(44, 44);
    put(48, algorithm); put(52, 1);
    put(56, 1); put(60, 16); put(64, 1); put(68, 28); put(72, 16);
    put(76, 32 | (16u << 16)); put(80, 32 | (32u << 16));
    output->data = bytes;
    output->size = sizeof(bytes);
    return 0;
}
