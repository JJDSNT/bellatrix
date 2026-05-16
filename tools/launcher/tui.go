package main

import (
	"fmt"
	"strings"

	tea "github.com/charmbracelet/bubbletea"
	"github.com/charmbracelet/lipgloss"
)

type launchResult struct {
	emuProfile     string
	kickstart      string
	adf            string
	displayMode    string
	bootArgs       string
	multicoreBuild bool
	multicoreLogs  bool
	btstack        bool
	usbstack       bool
	usbPointer     string
	emu68Boards    string
	cpuBackend     string
	fpuEnabled     bool
	z2RamSize      string
	serialBackend  string
	osd            bool
	launcher       bool
	cancelled      bool
}

type activePane int

const (
	paneKickstart activePane = iota
	paneADF
)

type model struct {
	roms []FileEntry
	adfs []FileEntry

	romCursor int
	adfCursor int
	active    activePane

	displayMode    string
	debugMode      string // "", "debug", "disassemble"
	multicoreBuild bool
	multicoreLogs  bool
	btstack        bool
	usbstack       bool
	usbPointer     string
	emu68Boards    string
	cpuBackend     string
	fpuEnabled     bool
	z2RamSize      string
	serialBackend  string
	osd            bool
	launcher       bool

	width     int
	height    int
	quitting  bool
	cancelled bool
}

func runLauncher(roms []FileEntry, adfs []FileEntry) (launchResult, error) {
	m := model{
		roms:           roms,
		adfs:           adfs,
		romCursor:      defaultROMIndex(roms),
		adfCursor:      0,
		active:         paneKickstart,
		displayMode:    "gtk",
		multicoreBuild: false,
		multicoreLogs:  false,
		btstack:        false,
		usbstack:       true,
		usbPointer:     "mouse",
		emu68Boards:    "legacy",
		cpuBackend:     "musashi",
		fpuEnabled:     true,
		z2RamSize:      "off",
		serialBackend:  "log",
		osd:            true,
		launcher:       true,
	}

	p := tea.NewProgram(m, tea.WithAltScreen())
	finalModel, err := p.Run()
	if err != nil {
		return launchResult{}, err
	}

	fm := finalModel.(model)
	if fm.cancelled {
		return launchResult{cancelled: true}, nil
	}

	selectedROM := fm.roms[fm.romCursor]
	kickstart := ""
	if !selectedROM.None {
		kickstart = selectedROM.Path
	}

	selectedADF := fm.adfs[fm.adfCursor]
	adf := ""
	if !selectedADF.None {
		adf = selectedADF.Path
	}

	return launchResult{
		emuProfile:     launcherProfileForCPUBackend(fm.cpuBackend),
		kickstart:      kickstart,
		adf:            adf,
		displayMode:    fm.displayMode,
		bootArgs:       buildBootArgs(fm.debugMode, fm.fpuEnabled),
		multicoreBuild: fm.multicoreBuild,
		multicoreLogs:  fm.multicoreLogs,
		btstack:        fm.btstack,
		usbstack:       fm.usbstack,
		usbPointer:     fm.usbPointer,
		emu68Boards:    fm.emu68Boards,
		cpuBackend:     fm.cpuBackend,
		fpuEnabled:     fm.fpuEnabled,
		z2RamSize:      fm.z2RamSize,
		serialBackend:  fm.serialBackend,
		osd:            fm.osd,
		launcher:       fm.launcher,
	}, nil
}

func nextZ2RamSize(current string) string {
	switch current {
	case "off":
		return "1"
	case "1":
		return "2"
	case "2":
		return "4"
	case "4":
		return "8"
	default:
		return "off"
	}
}

func defaultROMIndex(roms []FileEntry) int {
	for i, rom := range roms {
		if rom.None {
			continue
		}
		name := strings.ToLower(rom.Name)
		if strings.Contains(name, "ks13") {
			return i
		}
	}
	return 0
}

func nextEmu68BoardsMode(current string) string {
	if current == "legacy" {
		return "boards"
	}
	return "legacy"
}

func nextSerialBackend(current string) string {
	switch current {
	case "miniuart":
		return "pl011"
	case "pl011":
		return "log"
	default:
		return "miniuart"
	}
}

func launcherProfileForCPUBackend(cpuBackend string) string {
	if cpuBackend == "musashi" {
		return "bellatrix-musashi"
	}
	return "bellatrix"
}

func buildBootArgs(debugMode string, fpuEnabled bool) string {
	bootArgs := "enable_cache"
	if !fpuEnabled {
		bootArgs += " nofpu"
	}
	if debugMode != "" {
		bootArgs += " " + debugMode
	}
	return bootArgs
}

func (m model) Init() tea.Cmd {
	return nil
}

func (m model) Update(msg tea.Msg) (tea.Model, tea.Cmd) {
	switch msg := msg.(type) {
	case tea.WindowSizeMsg:
		m.width = msg.Width
		m.height = msg.Height
		return m, nil

	case tea.KeyMsg:
		switch msg.String() {
		case "ctrl+c", "q":
			m.cancelled = true
			m.quitting = true
			return m, tea.Quit

		case "tab":
			if m.active == paneKickstart {
				m.active = paneADF
			} else {
				m.active = paneKickstart
			}
			return m, nil

		case "up", "k":
			switch m.active {
			case paneKickstart:
				if m.romCursor > 0 {
					m.romCursor--
				}
			case paneADF:
				if m.adfCursor > 0 {
					m.adfCursor--
				}
			}
			return m, nil

		case "down", "j":
			switch m.active {
			case paneKickstart:
				if m.romCursor < len(m.roms)-1 {
					m.romCursor++
				}
			case paneADF:
				if m.adfCursor < len(m.adfs)-1 {
					m.adfCursor++
				}
			}
			return m, nil

		case "d":
			if m.displayMode == "gtk" {
				m.displayMode = "none"
			} else {
				m.displayMode = "gtk"
			}
			return m, nil

		case "b":
			switch m.debugMode {
			case "":
				m.debugMode = "debug"
			case "debug":
				m.debugMode = "disassemble"
			default:
				m.debugMode = ""
			}
			return m, nil

		case "m":
			m.multicoreBuild = !m.multicoreBuild
			return m, nil

		case "l":
			m.multicoreLogs = !m.multicoreLogs
			return m, nil

		case "t":
			m.btstack = !m.btstack
			return m, nil

		case "u":
			m.usbstack = !m.usbstack
			return m, nil

		case "p":
			if m.usbPointer == "mouse" {
				m.usbPointer = "tablet"
			} else {
				m.usbPointer = "mouse"
			}
			return m, nil

		case "e":
			m.emu68Boards = nextEmu68BoardsMode(m.emu68Boards)
			return m, nil

		case "c":
			if m.cpuBackend == "musashi" {
				m.cpuBackend = "emu68"
			} else {
				m.cpuBackend = "musashi"
			}
			return m, nil

		case "f":
			m.fpuEnabled = !m.fpuEnabled
			return m, nil

		case "z":
			m.z2RamSize = nextZ2RamSize(m.z2RamSize)
			return m, nil

		case "s":
			m.serialBackend = nextSerialBackend(m.serialBackend)
			return m, nil

		case "o":
			m.osd = !m.osd
			return m, nil

		case "n":
			m.launcher = !m.launcher
			return m, nil

		case "enter":
			m.quitting = true
			return m, tea.Quit
		}
	}

	return m, nil
}

func (m model) View() string {
	if m.width == 0 || m.height == 0 {
		return "Loading Bellatrix launcher..."
	}

	panel := m.renderPanel()

	return lipgloss.Place(
		m.width,
		m.height,
		lipgloss.Center,
		lipgloss.Center,
		panel,
	)
}

func (m model) renderPanel() string {
	var b strings.Builder

	header := lipgloss.JoinVertical(
		lipgloss.Center,
		headerTitleStyle.Render("BELLATRIX"),
		headerSubtitleStyle.Render("Raspberry Pi 3B • Bellatrix launcher"),
	)

	b.WriteString(headerBlockStyle.Render(header))
	b.WriteString("\n")

	b.WriteString(sectionTitleStyle.Render("Kickstart / payload"))
	if m.active == paneKickstart {
		b.WriteString(" ")
		b.WriteString(onBadgeStyle.Render("ACTIVE"))
	}
	b.WriteString("\n")

	for i, rom := range m.roms {
		line := "  " + rom.Name
		if i == m.romCursor {
			line = "> " + rom.Name
			b.WriteString(selectedItemStyle.Render(line))
		} else {
			b.WriteString(itemStyle.Render(line))
		}
		b.WriteString("\n")
	}

	b.WriteString("\n")
	b.WriteString(sectionTitleStyle.Render("DF0 floppy"))
	if m.active == paneADF {
		b.WriteString(" ")
		b.WriteString(onBadgeStyle.Render("ACTIVE"))
	}
	b.WriteString("\n")

	for i, adf := range m.adfs {
		line := "  " + adf.Name
		if i == m.adfCursor {
			line = "> " + adf.Name
			b.WriteString(selectedItemStyle.Render(line))
		} else {
			b.WriteString(itemStyle.Render(line))
		}
		b.WriteString("\n")
	}

	b.WriteString("\n")
	b.WriteString(sectionTitleStyle.Render("Options"))
	b.WriteString("\n")

	displayBadge := onBadgeStyle.Render("GTK")
	if m.displayMode == "none" {
		displayBadge = offBadgeStyle.Render("HEADLESS")
	}
	b.WriteString(fmt.Sprintf("%s %s", itemStyle.Render("Display:"), displayBadge))
	b.WriteString("\n")

	var debugBadge string
	switch m.debugMode {
	case "debug":
		debugBadge = onBadgeStyle.Render("DEBUG")
	case "disassemble":
		debugBadge = onBadgeStyle.Render("DISASM")
	default:
		debugBadge = offBadgeStyle.Render("OFF")
	}
	b.WriteString(fmt.Sprintf("%s %s", itemStyle.Render("Debug:"), debugBadge))
	b.WriteString("\n")

	multicoreBadge := offBadgeStyle.Render("OFF")
	if m.multicoreBuild {
		multicoreBadge = onBadgeStyle.Render("ON")
	}
	b.WriteString(fmt.Sprintf("%s %s", itemStyle.Render("Multicore build:"), multicoreBadge))
	b.WriteString("\n")

	logsBadge := offBadgeStyle.Render("OFF")
	if m.multicoreLogs {
		logsBadge = onBadgeStyle.Render("ON")
	}
	b.WriteString(fmt.Sprintf("%s %s", itemStyle.Render("Multicore logs:"), logsBadge))
	b.WriteString("\n")

	btstackBadge := offBadgeStyle.Render("OFF")
	if m.btstack {
		btstackBadge = onBadgeStyle.Render("ON")
	}
	b.WriteString(fmt.Sprintf("%s %s", itemStyle.Render("BTStack:"), btstackBadge))
	b.WriteString("\n")

	usbstackBadge := offBadgeStyle.Render("OFF")
	if m.usbstack {
		usbstackBadge = onBadgeStyle.Render("ON")
	}
	b.WriteString(fmt.Sprintf("%s %s", itemStyle.Render("USB stack:"), usbstackBadge))
	b.WriteString("\n")

	usbPointerBadge := offBadgeStyle.Render(strings.ToUpper(m.usbPointer))
	if m.usbstack {
		usbPointerBadge = onBadgeStyle.Render(strings.ToUpper(m.usbPointer))
	}
	b.WriteString(fmt.Sprintf("%s %s", itemStyle.Render("USB pointer:"), usbPointerBadge))
	b.WriteString("\n")

	boardsBadge := offBadgeStyle.Render("LEGACY")
	if m.emu68Boards == "boards" {
		boardsBadge = onBadgeStyle.Render("BOARDS")
	}
	b.WriteString(fmt.Sprintf("%s %s", itemStyle.Render("Emu68 boards:"), boardsBadge))
	b.WriteString("\n")

	cpuBackendBadge := offBadgeStyle.Render("EMU68")
	if m.cpuBackend == "musashi" {
		cpuBackendBadge = onBadgeStyle.Render("MUSASHI")
	}
	b.WriteString(fmt.Sprintf("%s %s", itemStyle.Render("CPU backend:"), cpuBackendBadge))
	b.WriteString("\n")

	fpuBadge := offBadgeStyle.Render("OFF")
	if m.fpuEnabled {
		fpuBadge = onBadgeStyle.Render("ON")
	}
	b.WriteString(fmt.Sprintf("%s %s", itemStyle.Render("FPU:"), fpuBadge))
	b.WriteString("\n")

	z2RamBadge := offBadgeStyle.Render("OFF")
	if m.z2RamSize != "off" {
		z2RamBadge = onBadgeStyle.Render(m.z2RamSize + "MB")
	}
	b.WriteString(fmt.Sprintf("%s %s", itemStyle.Render("Z2 RAM:"), z2RamBadge))
	b.WriteString("\n")

	var serialBadge string
	switch m.serialBackend {
	case "pl011":
		serialBadge = onBadgeStyle.Render("PL011")
	case "log":
		serialBadge = onBadgeStyle.Render("LOG")
	default:
		serialBadge = offBadgeStyle.Render("MINIUART")
	}
	b.WriteString(fmt.Sprintf("%s %s", itemStyle.Render("Serial:"), serialBadge))
	b.WriteString("\n")

	osdBadge := offBadgeStyle.Render("OFF")
	if m.osd {
		osdBadge = onBadgeStyle.Render("ON")
	}
	b.WriteString(fmt.Sprintf("%s %s", itemStyle.Render("OSD overlay:"), osdBadge))
	b.WriteString("\n")

	launcherBadge := offBadgeStyle.Render("OFF")
	if m.launcher {
		launcherBadge = onBadgeStyle.Render("ON")
	}
	b.WriteString(fmt.Sprintf("%s %s", itemStyle.Render("ADF launcher:"), launcherBadge))
	b.WriteString("\n\n")

	b.WriteString(sectionTitleStyle.Render("QEMU command"))
	b.WriteString("\n")
	b.WriteString(commandStyle.Render(m.qemuCommand()))
	b.WriteString("\n")

	b.WriteString(helpStyle.Render("↑/↓ Navigate • Tab Switch Section • D Display • B Debug • M Multicore • L Logs • T BTStack • U USB • P Pointer • E Boards • C CPU • F FPU • Z Z2 RAM • S Serial • O OSD • N Launcher • Enter Run • Q Quit"))

	return panelStyle.Render(b.String())
}

func (m model) qemuCommand() string {
	displayArg := "gtk,zoom-to-fit=on,window-close=on"
	if m.displayMode == "none" {
		displayArg = "none"
	}

	image := "emu68/install-bellatrix/Emu68.img"
	dtb := "emu68/install-bellatrix/bcm2710-rpi-3-b.dtb"
	if m.cpuBackend == "musashi" {
		image = "emu68/install-bellatrix-musashi/Emu68.img"
		dtb = "emu68/install-bellatrix-musashi/bcm2710-rpi-3-b.dtb"
	}

	bootArgs := buildBootArgs(m.debugMode, m.fpuEnabled)

	serialEnv := ""
	switch m.serialBackend {
	case "pl011":
		serialEnv = " BELLATRIX_SERIAL=pl011"
	case "log":
		serialEnv = " BELLATRIX_SERIAL=log"
	}

	base := fmt.Sprintf(
		`BELLATRIX_MULTICORE_BUILD=%s BELLATRIX_MULTICORE_LOGS=%s BELLATRIX_BTSTACK=%s BELLATRIX_USBSTACK=%s BELLATRIX_EMU68_BOARDS_MODE=%s BELLATRIX_OSD=%s BELLATRIX_LAUNCHER=%s%s qemu-system-aarch64 -M raspi3b -kernel %s -dtb %s -serial stdio -display %s -append "%s"%s`,
		boolEnv(m.multicoreBuild),
		boolEnv(m.multicoreLogs),
		boolEnv(m.btstack),
		boolEnv(m.usbstack),
		m.emu68Boards,
		boolEnv(m.osd),
		boolEnv(m.launcher),
		serialEnv,
		image,
		dtb,
		displayArg,
		bootArgs,
		qemuUSBDeviceArgs(m.usbstack, m.usbPointer),
	)

	selected := m.roms[m.romCursor]
	if selected.None {
		return base
	}

	cmd := fmt.Sprintf("%s -initrd %s", base, selected.Path)

	// Inject the ADF at physical 0x18000000 via the QEMU generic loader.
	// Bellatrix checks that address for the 'DOS' boot-block magic when the
	// EMMC init fails (no physical SD card in the QEMU environment).
	selectedADF := m.adfs[m.adfCursor]
	if !selectedADF.None {
		cmd = fmt.Sprintf("%s -device loader,file=%s,addr=0x18000000,force-raw=on",
			cmd, selectedADF.Path)
	}

	return cmd
}

func boolEnv(enabled bool) string {
	if enabled {
		return "1"
	}
	return "0"
}

func qemuUSBDeviceArgs(usbstack bool, usbPointer string) string {
	if usbstack {
		if usbPointer == "mouse" {
			return " -device usb-kbd -device usb-mouse"
		}
		return " -device usb-kbd -device usb-tablet"
	}
	return ""
}
