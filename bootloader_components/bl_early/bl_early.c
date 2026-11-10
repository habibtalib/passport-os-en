// Boot white-screen fix: at the very start of the second-stage bootloader, drive the backlight pin (GPIO21) low and enable output.
//
// Background: when the MCU pin floats, the backlight circuit lights the backlight, while the ST7789 panel is neither initialized nor
// has GRAM written - an undriven transmissive panel under backlight is just white. app_main() does
// already pull this pin low, but that is ~560ms after power-on; the second-stage bootloader loading/verifying the whole
// app image accounts for most of the white-screen window, which app-side code can never cover.
//
// Here we use the IDF bootloader hooks (bootloader_before_init / bootloader_after_init,
// both weak, overridden by strong definitions in this project) to move "drive backlight low" to the very start of the second-stage bootloader.
// Uses only ROM functions and direct register writes, no dependency on drivers or any runtime services (at the before_init stage
// even the flash cache is not up yet).
//
// At before_init the peripheral clock may not be on yet; to make sure it takes effect, write again in after_init (idempotent).

#include <stdint.h>

#include "esp_rom_gpio.h"
#include "esp_rom_sys.h"
#include "soc/gpio_reg.h"
#include "soc/gpio_sig_map.h"
#include "soc/soc.h"

// Keep consistent with BSP_LCD_BL in components/bsp/include/bsp_pins.h.
// The bootloader does not take part in main component dependencies, so it is hardcoded here (changing the backlight pin requires changing this too).
#define BSP_LCD_BL_EARLY 21

// Set to 1 to enable boot diagnostic output (prints GPIO21 out/oe/IE + floating level at each stage).
// Normally 0, leaving only one confirmation log line "driven low".
#define BL_EARLY_DIAG 0

static void backlight_drive_low(void) {
    // 1. Connect the pad to the GPIO matrix
    esp_rom_gpio_pad_select_gpio(BSP_LCD_BL_EARLY);
    // 2. Select the GPIO register itself as the output source (not some peripheral signal)
    esp_rom_gpio_connect_out_signal(BSP_LCD_BL_EARLY, SIG_GPIO_OUT_IDX, false, false);
    // 3. Write 0 first, then enable output: the reverse order would let residual high output leak out at the instant output is enabled
    REG_WRITE(GPIO_OUT_W1TC_REG, (1u << BSP_LCD_BL_EARLY));
    REG_WRITE(GPIO_ENABLE_W1TS_REG, (1u << BSP_LCD_BL_EARLY));
}

#if BL_EARLY_DIAG
// Read back the GPIO21 config to confirm "driven low" really took effect and was not changed back by a later stage.
// Reads registers only, touches no CSR - WARNING: ESP32-C3 **does not implement the standard cycle CSR (0xC00)**
// (SOC_CPU_HAS_CSR_PC=1, cycle counting uses private CSR 0x7e2). Reading 0xC00 early causes an immediate
// Illegal instruction -> bootloader reboots forever.
//   out=output level oe=output enable in=pin readback IE=input enable WPU/WPD=internal pull-up/pull-down
static void diag_dump(const char *where) {
    uint32_t out = REG_READ(GPIO_OUT_REG);
    uint32_t oe  = REG_READ(GPIO_ENABLE_REG);
    uint32_t in  = REG_READ(GPIO_IN_REG);
    uint32_t pin = REG_READ(GPIO_PIN21_REG);
    esp_rom_printf("[bl_early] %s out=%u oe=%u in=%u IE=%u WPU=%u WPD=%u\n",
                   where,
                   (unsigned)((out >> BSP_LCD_BL_EARLY) & 1u),
                   (unsigned)((oe >> BSP_LCD_BL_EARLY) & 1u),
                   (unsigned)((in >> BSP_LCD_BL_EARLY) & 1u),
                   (unsigned)((pin >> 8) & 1u),
                   (unsigned)((pin >> 7) & 1u),
                   (unsigned)((pin >> 6) & 1u));
}

// Measure "what level external circuitry pulls this pin to when floating".
// WARNING: esp_rom_gpio_pad_select_gpio() only switches MCU_SEL to the GPIO function, and does **not** enable
//   the input buffer (measured: IE is still 0 after the call, so reading GPIO_IN always gives 0, meaningless).
//   IE=1 must be set explicitly and internal pulls turned off, so the reading is the level "decided by external circuitry".
// in=1 -> the board (or the backlight driver EN pin) has a pull-up, floating lights the backlight = direct cause of the white screen.
static void diag_float_level(void) {
    esp_rom_gpio_pad_select_gpio(BSP_LCD_BL_EARLY);
    REG_SET_BIT(GPIO_PIN21_REG, 1u << 8);                 // IE=1 enable input buffer
    REG_CLR_BIT(GPIO_PIN21_REG, (1u << 7) | (1u << 6));   // WPU=0 WPD=0
    uint32_t pin = REG_READ(GPIO_PIN21_REG);
    uint32_t in  = REG_READ(GPIO_IN_REG);

    // Then enable the internal pull-down (~45k ohm) and read once more: if still high, the external pull-up is strong (not just
    // weak leakage), and floating will certainly light the backlight.
    REG_SET_BIT(GPIO_PIN21_REG, 1u << 6);                 // WPD=1
    uint32_t in_pd = REG_READ(GPIO_IN_REG);
    REG_CLR_BIT(GPIO_PIN21_REG, 1u << 6);                 // restore

    esp_rom_printf("[bl_early] float IE=%u WPU=%u WPD=%u in=%u in_pd=%u\n",
                   (unsigned)((pin >> 8) & 1u),
                   (unsigned)((pin >> 7) & 1u),
                   (unsigned)((pin >> 6) & 1u),
                   (unsigned)((in >> BSP_LCD_BL_EARLY) & 1u),
                   (unsigned)((in_pd >> BSP_LCD_BL_EARLY) & 1u));
}
#endif

// The IDF bootloader uses `-u bootloader_hooks_include` to force-keep this symbol.
// Reason: the hook is a **weak reference** in bootloader_start.c, and the linker does **not** extract archive members
// for a "weak undefined" symbol - merely defining bootloader_before_init() would be
// dropped (measured in disassembly: the two hook calls in call_start_cpu0 compile to
// `li a5,0 / beqz` and never execute). Defining this symbol pulls this object file into the link.
void bootloader_hooks_include(void);
void bootloader_hooks_include(void) {}

void bootloader_before_init(void) {
#if BL_EARLY_DIAG
    // 1) What level external circuitry pulls it to when floating - to determine the cause of the white screen
    diag_float_level();
    diag_dump("pre_hook ");
#endif
    backlight_drive_low();
#if BL_EARLY_DIAG
    diag_dump("post_hook");
#endif
}

void bootloader_after_init(void) {
    backlight_drive_low();
#if BL_EARLY_DIAG
    diag_dump("after_init");
#else
    // The bootloader console is ready now; leave one line of trace so we can later confirm the hook really took effect.
    esp_rom_printf("[bl_early] backlight GPIO%d driven low\n", BSP_LCD_BL_EARLY);
#endif
}
