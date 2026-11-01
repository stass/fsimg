/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
/* The fortified pread wrapper would be renamed into the seam's own definition
 * and hide the fake endpoint.
 */
#undef _FORTIFY_SOURCE

/* Compile the production constructors and adapter against a disposable fake
 * host endpoint. The installed library has no device-test hooks or switches.
 */
#define fsimg_host_fstat fsimg_test_device_fstat
#define fsimg_host_disk_sector fsimg_test_device_sector
#define fsimg_host_disk_size fsimg_test_device_size
#define pread fsimg_test_device_pread
#include "../lib/io/input.c"
#include "../lib/io/io.c"
