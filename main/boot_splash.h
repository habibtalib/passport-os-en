// Boot animation: green-phosphor style - "PASSPORT OS" typewriter cursor + green-dot progress bar.
// Same palette as Home (black background / phosphor green), about 0.9s; paces itself, the caller must not hold the LVGL lock.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Show the boot animation and block for about 0.9s. Takes/releases the LVGL lock internally.
void boot_splash_run(void);

// Call after Home has finished loading: delete the boot screen object (the animation screen is no longer visible afterwards).
void boot_splash_end(void);

#ifdef __cplusplus
}
#endif
