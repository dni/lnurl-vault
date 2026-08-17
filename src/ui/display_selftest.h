#ifndef LNURLVAULT_DISPLAY_SELFTEST_H
#define LNURLVAULT_DISPLAY_SELFTEST_H

/* Boot-time panel diagnostics. Each is a no-op unless its flag is defined;
 * see display_selftest.c for what each step isolates. */

#ifdef LNURLVAULT_DISPLAY_SELFTEST
void display_selftest_run(void);
#else
static inline void display_selftest_run(void) {}
#endif

#ifdef LNURLVAULT_QR_SELFTEST
void qr_selftest_run(void);
#else
static inline void qr_selftest_run(void) {}
#endif

#endif
