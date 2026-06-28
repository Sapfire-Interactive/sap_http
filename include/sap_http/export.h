#pragma once

// dllexport when building the DLL, dllimport for consumers, no-op for the
// static lib / non-Windows.
#if defined(SAP_HTTP_STATIC_DEFINE) || !defined(_WIN32)
#define SAP_HTTP_API
#elif defined(SAP_HTTP_EXPORTS)
#define SAP_HTTP_API __declspec(dllexport)
#else
#define SAP_HTTP_API __declspec(dllimport)
#endif
