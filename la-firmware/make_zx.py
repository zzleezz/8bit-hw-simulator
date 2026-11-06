"""Make the zx firmware from gusmanb's LogicAnalyzer firmware (GPL-3).

    git clone -b version/v6_0 https://github.com/gusmanb/logicanalyzer.git gusmanb
    py make_zx.py gusmanb/Firmware/LogicAnalyzer_V2 zx
    sh build.sh zx            (zx/build/LogicAnalyzer.uf2)

Tested on commit 6120d4c (2025-03-06). The changes, for a ZX Spectrum's
bus (8bit-hw-simulator; AI-generated with Claude Code):

1. The complex (pattern) trigger needs its pattern on 5 reads in a row,
   60 ns at 200 MHz, not one. As the Spectrum's address lines change they
   don't switch together, and for a few ns show other addresses, which
   fired the trigger (going 11DD -> D1E2 showed 11E2). A Z80 address stays
   500 ns or more.
2. A capture that's done is sent at once: the main loop looked every 2 s.
3. Command 10 answers ZX_FIRMWARE:1 (the stock firmware: ERR_UNKNOWN_MSG).
4. The build: Pico 2, not overclocked (100 MHz, as the stock Pico 2
   build), and without the Pico VS Code extension's file.

Same protocol otherwise: gusmanb's own program works with it.
"""
import os, shutil, sys

src, dst = sys.argv[1], sys.argv[2]
if os.path.exists(dst):
    shutil.rmtree(dst)
shutil.copytree(src, dst, ignore=shutil.ignore_patterns('build', 'publish'))


def patch(name, pairs):
    p = os.path.join(dst, name)
    s = open(p, encoding='utf-8', errors='surrogateescape', newline='').read()
    crlf = '\r\n' in s
    s = s.replace('\r\n', '\n')
    s = '\n'.join(l.rstrip() for l in s.split('\n'))     # trailing spaces don't count
    for old, new in pairs:
        if s.count(old) != 1:
            sys.exit(f"{name}: can't find (or more than once): {old[:70]!r}")
        s = s.replace(old, new)
    if crlf:
        s = s.replace('\n', '\r\n')
    open(p, 'w', encoding='utf-8', errors='surrogateescape', newline='').write(s)


patch('CMakeLists.txt', [('include(${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)',
                          'include(${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake OPTIONAL)')])
patch('LogicAnalyzer_Build_Settings.cmake', [('set(TURBO_MODE 1)', 'set(TURBO_MODE 0)')])

patch('LogicAnalyzer_Capture.c', [
('''#define COMPLEX_TRIGGER_wrap_target 0
#define COMPLEX_TRIGGER_wrap 8

uint16_t COMPLEX_TRIGGER_program_instructions[] = {
            //     .wrap_target
    0x80a0, //  0: pull   block
    0x6020, //  1: out    x, 32
    0xe000, //  2: set    pins, 0
    0xc007, //  3: irq    nowait 7
    0xa0e0, //  4: mov    osr, pins
    0x6044, //  5: out    y, 4
    0x00a4, //  6: jmp    x != y, 4
    0xe001, //  7: set    pins, 1
    0x0008, //  8: jmp    8
            //     .wrap
};

struct pio_program COMPLEX_TRIGGER_program = {
    .instructions = COMPLEX_TRIGGER_program_instructions,
    .length = 9,
    .origin = -1,
};''', '''/* 8bit-hw-simulator change (zx firmware, AI-generated with Claude Code):
 * the pattern must be read COMPLEX_TRIGGER_READS times in a row (3
 * instructions each, 15 ns at 200 MHz), not once. On a ZX Spectrum's address
 * bus the lines don't switch together, and for a few ns a change shows
 * addresses in between, which fired the trigger (11DD -> D1E2 showed 11E2).
 * A real Z80 address stays for 500 ns or more. 5 reads, as the trigger and
 * COMPLEX_CAPTURE (9) must fit the PIO's 32 instructions: 6 didn't, and
 * pio_add_program (SDK 2.0) returns an error then, it doesn't stop. */
#define COMPLEX_TRIGGER_READS 5
#define COMPLEX_TRIGGER_wrap_target 0
#define COMPLEX_TRIGGER_wrap (4 + 3 * COMPLEX_TRIGGER_READS + 1)

uint16_t COMPLEX_TRIGGER_program_instructions[] = {
            //     .wrap_target
    0x80a0, //  0: pull   block
    0x6020, //  1: out    x, 32
    0xe000, //  2: set    pins, 0
    0xc007, //  3: irq    nowait 7
    0xa0e0, //  4: mov    osr, pins
    0x6044, //  5: out    y, 4 (the pin count, set below)
    0x00a4, //  6: jmp    x != y, 4
    0xa0e0, //  7: mov    osr, pins
    0x6044, //  8: out    y, 4
    0x00a4, //  9: jmp    x != y, 4
    0xa0e0, // 10: mov    osr, pins
    0x6044, // 11: out    y, 4
    0x00a4, // 12: jmp    x != y, 4
    0xa0e0, // 13: mov    osr, pins
    0x6044, // 14: out    y, 4
    0x00a4, // 15: jmp    x != y, 4
    0xa0e0, // 16: mov    osr, pins
    0x6044, // 17: out    y, 4
    0x00a4, // 18: jmp    x != y, 4
    0xe001, // 19: set    pins, 1
    0x0014, // 20: jmp    20
            //     .wrap
};

struct pio_program COMPLEX_TRIGGER_program = {
    .instructions = COMPLEX_TRIGGER_program_instructions,
    .length = 4 + 3 * COMPLEX_TRIGGER_READS + 2,
    .origin = -1,
};'''),
('''    COMPLEX_TRIGGER_program_instructions[5] = 0x6040 | triggerPinCount;''',
'''    for (int r = 0; r < COMPLEX_TRIGGER_READS; r++)
        COMPLEX_TRIGGER_program_instructions[5 + 3 * r] = 0x6040 | triggerPinCount;'''),
('''    triggerOffset = pio_add_program(capturePIO, &COMPLEX_TRIGGER_program);''',
'''    if (!pio_can_add_program(capturePIO, &COMPLEX_TRIGGER_program))
        return false;   /* CAPTURE_ERROR, not a trigger that never comes */
    triggerOffset = pio_add_program(capturePIO, &COMPLEX_TRIGGER_program);'''),
])

patch('LogicAnalyzer.c', [
('''            else
            {
                LED_OFF();
                sleep_ms(1000);

                //Check for cancel request
                if(processCancel())
                {
                    //Stop capture
                    StopCapture();
                    capturing = false;
                    LED_ON();
                }
                else
                {
                    LED_ON();
                    #ifdef SUPPORTS_COMPLEX_TRIGGER
                    check_fast_interrupt();
                    #endif
                    sleep_ms(1000);
                }
            }''', '''            else
            {
                /* 8bit-hw-simulator change: look every ms, not every 2 s,
                 * so the host knows when the trigger came (the LED still
                 * blinks once a second). */
                static uint32_t ticks = 0;
                if (ticks % 2000 == 0) LED_OFF();
                else if (ticks % 2000 == 1000) LED_ON();
                ticks++;
                sleep_ms(1);

                //Check for cancel request
                if(processCancel())
                {
                    //Stop capture
                    StopCapture();
                    capturing = false;
                    LED_ON();
                }
                else
                {
                    #ifdef SUPPORTS_COMPLEX_TRIGGER
                    check_fast_interrupt();
                    #endif
                }
            }'''),
('''                    default:

                        sendResponse("ERR_UNKNOWN_MSG\\n", fromWiFi); //Unknown message''',
'''                    case 10: //8bit-hw-simulator: which firmware

                        sendResponse("ZX_FIRMWARE:1\\n", fromWiFi);
                        break;

                    default:

                        sendResponse("ERR_UNKNOWN_MSG\\n", fromWiFi); //Unknown message'''),
])
print(f"{dst}: made from {src}")
