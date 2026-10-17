// Small on-console prerequisite check. This is not the RPCS3 emulator.
#include "Crypto/sha256.h"

#include <orbis/libkernel.h>

#include <cstdio>
#include <cstring>
#include <sys/stat.h>

// sha256.cpp uses this RPCS3 crypto helper. Defining it here keeps the probe
// independent of the rest of the emulator core.
void mbedtls_zeroize(void* data, size_t size)
{
	auto* bytes = static_cast<volatile unsigned char*>(data);
	while (size--)
	{
		*bytes++ = 0;
	}
}

int main()
{
	static constexpr unsigned char expected[32] = {
		0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea,
		0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
		0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
		0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad,
	};
	unsigned char digest[32]{};
	const unsigned char input[] = {'a', 'b', 'c'};
	mbedtls_sha256_context context{};
	mbedtls_sha256_init(&context);
	const bool passed = mbedtls_sha256_starts_ret(&context, 0) == 0 &&
		mbedtls_sha256_update_ret(&context, input, sizeof(input)) == 0 &&
		mbedtls_sha256_finish_ret(&context, digest) == 0 &&
		std::memcmp(digest, expected, sizeof(expected)) == 0;
	mbedtls_sha256_free(&context);

	const char* message = passed
		? "RPCS3 PS4 probe: SHA-256 original RPCS3 code PASS\n"
		: "RPCS3 PS4 probe: SHA-256 original RPCS3 code FAIL\n";
	sceKernelDebugOutText(0, message);
	::mkdir("/data/rpcs3", 0777);
	if (FILE* file = std::fopen("/data/rpcs3/port_probe.txt", "wb"))
	{
		std::fwrite(message, 1, std::strlen(message), file);
		std::fclose(file);
	}
	// Stay alive like the OpenOrbis homebrew samples (an earlier probe that
	// returned at once ended in a shell error). Close the app from the PS4 home
	// menu after reading the result over FTP.
	for (;;)
	{
		sceKernelUsleep(1000000);
	}
}
