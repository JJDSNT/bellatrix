// bellatrix/tools/launcher/main.go
package main

import (
	"fmt"
	"os"
)

func main() {
	if len(os.Args) < 2 {
		fmt.Fprintln(os.Stderr, "usage: bellatrix-launcher <roms-dir> [output-file]")
		os.Exit(1)
	}

	romsDir := os.Args[1]
	outputFile := ""
	if len(os.Args) >= 3 {
		outputFile = os.Args[2]
	}

	roms, err := scanROMs(romsDir)
	if err != nil {
		fmt.Fprintf(os.Stderr, "failed to scan ROMs: %v\n", err)
		os.Exit(1)
	}

	if len(roms) == 0 {
		fmt.Fprintf(os.Stderr, "no ROM files found in %s\n", romsDir)
		os.Exit(1)
	}

	result, err := runLauncher(roms)
	if err != nil {
		fmt.Fprintf(os.Stderr, "launcher failed: %v\n", err)
		os.Exit(1)
	}

	if result.cancelled {
		os.Exit(130)
	}

	output := fmt.Sprintf(
		"EMU_PROFILE=%s\nKICKSTART=%s\nDISPLAY_MODE=%s\nBOOTARGS=%s\n",
		result.emuProfile,
		result.kickstart,
		result.displayMode,
		result.bootArgs,
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
