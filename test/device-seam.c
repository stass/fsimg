/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
/* Compile the production constructors and adapter against a disposable fake
 * host endpoint. The installed library has no device-test hooks or switches.
 *
 * With _FORTIFY_SOURCE, whether a toolchain enables it by default, as
 * Ubuntu's gcc does, or the build passes it, glibc defines each fortified
 * function, pread among them, as an always-inline wrapper around the real
 * call. Renaming such a function below would turn its wrapper into an
 * inline definition of the hook, so the adapter would bypass the fake
 * endpoint; the seam therefore compiles unfortified.
 */
#undef _FORTIFY_SOURCE
#define fsimg_host_fstat fsimg_test_device_fstat
#define fsimg_host_disk_sector fsimg_test_device_sector
#define fsimg_host_disk_size fsimg_test_device_size
#define pread fsimg_test_device_pread
#include "../lib/io/input.c"
#include "../lib/io/io.c"
