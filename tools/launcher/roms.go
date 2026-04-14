// bellatrix/tools/launcher/roms.go
package main

import (
	"os"
	"path/filepath"
	"sort"
	"strings"
)

type ROM struct {
	Name string
	Path string
	None bool
}

func scanROMs(dir string) ([]ROM, error) {
	entries, err := os.ReadDir(dir)
	if err != nil {
		return nil, err
	}

	var roms []ROM

	roms = append(roms, ROM{
		Name: "[No Kickstart]",
		Path: "",
		None: true,
	})

	for _, entry := range entries {
		if entry.IsDir() {
			continue
		}

		name := entry.Name()
		lower := strings.ToLower(name)

		if !isROMFile(lower) {
			continue
		}

		roms = append(roms, ROM{
			Name: name,
			Path: filepath.Join(dir, name),
		})
	}

	sort.Slice(roms[1:], func(i, j int) bool {
		return strings.ToLower(roms[i+1].Name) < strings.ToLower(roms[j+1].Name)
	})

	return roms, nil
}

func isROMFile(name string) bool {
	return strings.HasSuffix(name, ".rom") || strings.HasSuffix(name, ".bin")
}
