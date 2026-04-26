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
	/* Run TUI                                                               */
	/* --------------------------------------------------------------------- */

	result, err := runLauncher(roms, adfs)
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
		"EMU_PROFILE=%s\nKICKSTART=%s\nDISPLAY_MODE=%s\nBOOTARGS=%s\nADF=%s\n",
		result.emuProfile,
		result.kickstart,
		result.displayMode,
		result.bootArgs,
		result.adf,
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
