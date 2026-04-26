// bellatrix/tools/launcher/scan.go
package main

import (
	"os"
	"path/filepath"
	"sort"
	"strings"
)

/*
 * FileEntry — entrada genérica de arquivo selecionável na TUI.
 *
 * Usado para:
 *  - Kickstart (ROM)
 *  - Floppy (ADF)
 *  - Futuro: HDF, ISO
 */
type FileEntry struct {
	Name string
	Path string
	None bool
}

/* ------------------------------------------------------------------------- */
/* ROM scan (Kickstart)                                                      */
/* ------------------------------------------------------------------------- */

func scanROMs(dir string) ([]FileEntry, error) {
	entries, err := os.ReadDir(dir)
	if err != nil {
		return nil, err
	}

	var list []FileEntry

	list = append(list, FileEntry{
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

		list = append(list, FileEntry{
			Name: name,
			Path: filepath.Join(dir, name),
		})
	}

	sort.Slice(list[1:], func(i, j int) bool {
		return strings.ToLower(list[i+1].Name) < strings.ToLower(list[j+1].Name)
	})

	return list, nil
}

/* ------------------------------------------------------------------------- */
/* ADF scan (floppy DF0)                                                     */
/* ------------------------------------------------------------------------- */

func scanADFs(dir string) ([]FileEntry, error) {
	entries, err := os.ReadDir(dir)
	if err != nil {
		// pasta inexistente não é erro fatal
		return []FileEntry{
			{
				Name: "[No disk]",
				Path: "",
				None: true,
			},
		}, nil
	}

	var list []FileEntry

	list = append(list, FileEntry{
		Name: "[No disk]",
		Path: "",
		None: true,
	})

	for _, entry := range entries {
		if entry.IsDir() {
			continue
		}

		name := entry.Name()
		lower := strings.ToLower(name)

		if !isADFFile(lower) {
			continue
		}

		list = append(list, FileEntry{
			Name: name,
			Path: filepath.Join(dir, name),
		})
	}

	sort.Slice(list[1:], func(i, j int) bool {
		return strings.ToLower(list[i+1].Name) < strings.ToLower(list[j+1].Name)
	})

	return list, nil
}

/* ------------------------------------------------------------------------- */
/* FUTURO: HDF scan                                                          */
/* ------------------------------------------------------------------------- */

func scanHDFs(dir string) ([]FileEntry, error) {
	entries, err := os.ReadDir(dir)
	if err != nil {
		return []FileEntry{
			{
				Name: "[No hard disk]",
				Path: "",
				None: true,
			},
		}, nil
	}

	var list []FileEntry

	list = append(list, FileEntry{
		Name: "[No hard disk]",
		Path: "",
		None: true,
	})

	for _, entry := range entries {
		if entry.IsDir() {
			continue
		}

		name := entry.Name()
		lower := strings.ToLower(name)

		if !isHDFFile(lower) {
			continue
		}

		list = append(list, FileEntry{
			Name: name,
			Path: filepath.Join(dir, name),
		})
	}

	sort.Slice(list[1:], func(i, j int) bool {
		return strings.ToLower(list[i+1].Name) < strings.ToLower(list[j+1].Name)
	})

	return list, nil
}

/* ------------------------------------------------------------------------- */
/* FUTURO: ISO scan                                                          */
/* ------------------------------------------------------------------------- */

func scanISOs(dir string) ([]FileEntry, error) {
	entries, err := os.ReadDir(dir)
	if err != nil {
		return []FileEntry{
			{
				Name: "[No CD-ROM]",
				Path: "",
				None: true,
			},
		}, nil
	}

	var list []FileEntry

	list = append(list, FileEntry{
		Name: "[No CD-ROM]",
		Path: "",
		None: true,
	})

	for _, entry := range entries {
		if entry.IsDir() {
			continue
		}

		name := entry.Name()
		lower := strings.ToLower(name)

		if !isISOFile(lower) {
			continue
		}

		list = append(list, FileEntry{
			Name: name,
			Path: filepath.Join(dir, name),
		})
	}

	sort.Slice(list[1:], func(i, j int) bool {
		return strings.ToLower(list[i+1].Name) < strings.ToLower(list[j+1].Name)
	})

	return list, nil
}

/* ------------------------------------------------------------------------- */
/* File type helpers                                                         */
/* ------------------------------------------------------------------------- */

func isROMFile(name string) bool {
	return strings.HasSuffix(name, ".rom") ||
		strings.HasSuffix(name, ".bin")
}

func isADFFile(name string) bool {
	return strings.HasSuffix(name, ".adf")
}

func isHDFFile(name string) bool {
	return strings.HasSuffix(name, ".hdf")
}

func isISOFile(name string) bool {
	return strings.HasSuffix(name, ".iso")
}
