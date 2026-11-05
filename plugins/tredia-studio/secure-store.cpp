#include "secure-store.hpp"

#ifdef _WIN32
#include <windows.h>
#include <wincrypt.h>
#include <dpapi.h>

QByteArray protectSecret(const QByteArray &plain)
{
	if (plain.isEmpty())
		return {};
	DATA_BLOB in{static_cast<DWORD>(plain.size()), reinterpret_cast<BYTE *>(const_cast<char *>(plain.data()))};
	DATA_BLOB out{};
	if (!CryptProtectData(&in, L"Tredia Studio", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out))
		return {};
	QByteArray result(reinterpret_cast<const char *>(out.pbData), static_cast<qsizetype>(out.cbData));
	LocalFree(out.pbData);
	return result.toBase64();
}

QByteArray unprotectSecret(const QByteArray &stored)
{
	if (stored.isEmpty())
		return {};
	QByteArray blob = QByteArray::fromBase64(stored);
	DATA_BLOB in{static_cast<DWORD>(blob.size()), reinterpret_cast<BYTE *>(blob.data())};
	DATA_BLOB out{};
	if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out))
		return {};
	QByteArray result(reinterpret_cast<const char *>(out.pbData), static_cast<qsizetype>(out.cbData));
	LocalFree(out.pbData);
	return result;
}
#else
QByteArray protectSecret(const QByteArray &plain)
{
	return plain.toBase64();
}

QByteArray unprotectSecret(const QByteArray &stored)
{
	return QByteArray::fromBase64(stored);
}
#endif
