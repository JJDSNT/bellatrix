// bellatrix/tools/launcher/main.go
package main

import (
	"fmt"
	"os"
	"strings"
)

func main() {
	if len(os.Args) < 2 {
		fmt.Fprintln(os.Stderr, "usage: bellatrix-launcher <roms-dir> [output-file]")
		os.Exit(1)
	}

	romsDir := os.Args[1]

	// Convenção: disks fica ao lado de roms
	disksDir := strings.TrimSuffix(romsDir, "/") + "/../disks"

	outputFile := ""
	if len(os.Args) >= 3 {
		outputFile = os.Args[2]
	}

	/* --------------------------------------------------------------------- */
	/* Scan ROMs                                                             */
	/* --------------------------------------------------------------------- */

	roms, err := scanROMs(romsDir)
	if err != nil {
		fmt.Fprintf(os.Stderr, "failed to scan ROMs: %v\n", err)
		os.Exit(1)
	}

	if len(roms) == 0 {
		fmt.Fprintf(os.Stderr, "no ROM files found in %s\n", romsDir)
		os.Exit(1)
	}

	/* --------------------------------------------------------------------- */
	/* Scan ADFs                                                             */
	/* --------------------------------------------------------------------- */

	adfs, err := scanADFs(disksDir)
	if err != nil {
		fmt.Fprintf(os.Stderr, "failed to scan ADFs: %v\n", err)
		os.Exit(1)
	}

	/* --------------------------------------------------------------------- */
	/* Scan ISOs                                                             */
	/* --------------------------------------------------------------------- */

	isos, err := scanISOs(disksDir)
	if err != nil {
		fmt.Fprintf(os.Stderr, "failed to scan ISOs: %v\n", err)
		os.Exit(1)
	}

	/* --------------------------------------------------------------------- */
	/* Run TUI                                                               */
	/* --------------------------------------------------------------------- */

	result, err := runLauncher(roms, adfs, isos)
	if err != nil {
		fmt.Fprintf(os.Stderr, "launcher failed: %v\n", err)
		os.Exit(1)
	}

	if result.cancelled {
		os.Exit(130)
	}

	/* --------------------------------------------------------------------- */
	/* Output                                                                */
	/* --------------------------------------------------------------------- */

	output := fmt.Sprintf(
		"EMU_PROFILE=%s\nKICKSTART=%s\nDISPLAY_MODE=%s\nBOOTARGS=%s\nADF=%s\nISO=%s\nBELLATRIX_MULTICORE_BUILD=%s\nBELLATRIX_MULTICORE_LOGS=%s\nBELLATRIX_BTSTACK=%s\nBELLATRIX_USBSTACK=%s\nBELLATRIX_USB_POINTER=%s\nBELLATRIX_EMU68_BOARDS_MODE=%s\nBELLATRIX_CHIPSET_BACKEND=%s\nBELLATRIX_RIGEL_TRACE=%s\nBELLATRIX_Z2_RAM_SIZE=%s\nBELLATRIX_SERIAL=%s\nBELLATRIX_OSD=%s\nBELLATRIX_LAUNCHER=%s\n",
		result.emuProfile,
		result.kickstart,
		result.displayMode,
		result.bootArgs,
		result.adf,
		result.iso,
		boolEnv(result.multicoreBuild),
		boolEnv(result.multicoreLogs),
		boolEnv(result.btstack),
		boolEnv(result.usbstack),
		result.usbPointer,
		result.emu68Boards,
		result.chipsetBackend,
		boolEnv(result.rigelTrace),
		result.z2RamSize,
		result.serialBackend,
		boolEnv(result.osd),
		boolEnv(result.launcher),
	)

	if outputFile != "" {
		if err := os.WriteFile(outputFile, []byte(output), 0o644); err != nil {
			fmt.Fprintf(os.Stderr, "failed to write output file: %v\n", err)
			os.Exit(1)
		}
		return
	}

	fmt.Print(output)
}
