#pragma once

/* Translated text from data/locale/<lang>.ini. */

#include <obs-module.h>

#include <QString>

namespace vertical {

inline QString T(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

inline QString T(const QString &key)
{
	return T(key.toUtf8().constData());
}

} // namespace vertical
