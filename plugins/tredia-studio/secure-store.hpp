#pragma once

#include <QByteArray>

/* Secrets at rest (the Tredia sign-in, stream keys of "from this PC" destinations):
 * encrypted for the current Windows user with DPAPI, stored as base64. */
QByteArray protectSecret(const QByteArray &plain);
QByteArray unprotectSecret(const QByteArray &stored);
