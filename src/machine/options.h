/*
 * src/machine/options.h
 *
 * What the command line asks this machine to be.
 *
 * Emu68 hands Bellatrix the boot arguments before it decides anything from
 * them (patches/emu68/0025). This is where they are read, and it is the only
 * place: everything else asks a question here rather than knowing the spelling
 * of a token.
 */

#ifndef BELLATRIX_MACHINE_OPTIONS_H
#define BELLATRIX_MACHINE_OPTIONS_H

#ifdef __cplusplus
extern "C" {
#endif

/* Called by Emu68's parse_cmdline (patches/emu68/0025). May be called twice. */
void bellatrix_parse_cmdline(const char *cmdline);

/*
 * Is the classic chipset part of this machine?
 *
 * Answers 0 on an image built without it, whatever the command line says, so
 * a caller never has to test CONFIG_RIGEL as well.
 */
int bellatrix_rigel_enabled(void);

/* Did the command line ask for the fault-driven vector page? */
int bellatrix_vecpage_trapped(void);

/*
 * Say what was decided, once, from machine_init().
 *
 * Not at parse time: parse_cmdline runs twice on some boots, and a machine
 * that reports what it *is* has to report the disabled case too -- a chipset
 * that was never asked for and a chipset that failed to initialise produce the
 * same silence otherwise.
 */
void bellatrix_options_report(void);

#ifdef __cplusplus
}
#endif

#endif /* BELLATRIX_MACHINE_OPTIONS_H */
