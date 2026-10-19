/*
	PSFlyCast - HTTP(S) with a TLS of the title's own (mbedTLS).

	Copyright 2026 Press5elect and the PSFlyCast contributors
	SPDX-License-Identifier: GPL-3.0-or-later

	The console's HTTPS client (libSceHttp2 with libSceSsl) refuses every public
	certificate with 0x8095f00c once the title has left its sandbox through the
	sandbox elevator (USB drives, the disc drive), as BlackBearReloaded's
	ps5-native-app-boilerplate found and documents in docs/CURL.md, where the
	answer is a TLS stack in the app. This is that stack, from PSSwanStation's
	(Press5elect, the same answer to the same refusal): a socket of its own,
	mbedTLS checking the server's certificate against the console's list
	(/system/common/cert/CA_LIST.cer) and the title's copy of Mozilla's
	(certs/cacert.pem), and HTTP/1.1 with redirects and chunked answers. Here
	it also posts, with headers, for RetroAchievements. ps5_covers.cpp decides
	when it is used: once the sandbox has been left, or after the console's
	client refused a certificate.
*/
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace ps5::tls
{
// A certificate list read beside the console's (PEM or DER).
void addCertificateFile(const std::string& file);
// What it says about itself (the certificates read, a failure's reason), for the boot log.
void setLog(void (*log)(const char *message));
// What the requests say they come from (User-Agent).
void setUserAgent(const std::string& agent);

// One request. The answer's body goes to `out`: for a 2xx status up to 8 MiB,
// for any other only when anyStatus (a service's reason), up to 64 KiB. The
// status, or -1 with `why` set when no answer came. `seconds` limits the whole of it.
int request(const char *method, const std::string& url, const std::vector<std::pair<std::string, std::string>>& headers,
		const void *body, size_t bodySize, std::vector<uint8_t>& out, unsigned seconds, bool anyStatus,
		std::string *why = nullptr);
// A GET written to `file` as it arrives (anything but 200 leaves no file).
// `progress` is told how much has arrived and stops it by answering false.
// The status, -1 when it failed, -2 when progress stopped it.
int download(const std::string& url, const std::string& file, const std::function<bool(uint64_t done)>& progress,
		std::string *why = nullptr);
}
