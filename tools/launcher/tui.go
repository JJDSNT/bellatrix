package main

import (
	"fmt"
	"strings"

	tea "github.com/charmbracelet/bubbletea"
	"github.com/charmbracelet/lipgloss"
)

type launchResult struct {
	emuProfile  string
	kickstart   string
	displayMode string
	bootArgs    string
	cancelled   bool
}

type model struct {
	roms        []ROM
	cursor      int
	displayMode string
	emuProfile  string
	debugMode   string // "", "debug", "disassemble"
	width       int
	height      int
	quitting    bool
	cancelled   bool
}

func runLauncher(roms []ROM) (launchResult, error) {
	m := model{
		roms:        roms,
		cursor:      defaultROMIndex(roms),
		displayMode: "gtk",
		emuProfile:  "bellatrix",
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

	selected := fm.roms[fm.cursor]
	kickstart := ""
	if !selected.None {
		kickstart = selected.Path
	}

	bootArgs := "enable_cache"
	if fm.debugMode != "" {
		bootArgs = "enable_cache " + fm.debugMode
	}

	return launchResult{
		emuProfile:  fm.emuProfile,
		kickstart:   kickstart,
		displayMode: fm.displayMode,
		bootArgs:    bootArgs,
	}, nil
}

func defaultROMIndex(roms []ROM) int {
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

		case "up", "k":
			if m.cursor > 0 {
				m.cursor--
			}
			return m, nil

		case "down", "j":
			if m.cursor < len(m.roms)-1 {
				m.cursor++
			}
			return m, nil

		case "d":
			if m.displayMode == "gtk" {
				m.displayMode = "none"
			} else {
				m.displayMode = "gtk"
			}
			return m, nil

		case "e":
			if m.emuProfile == "bellatrix" {
				m.emuProfile = "emu68"
			} else {
				m.emuProfile = "bellatrix"
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
		headerSubtitleStyle.Render("Raspberry Pi 3B • Emu68 launcher"),
	)

	b.WriteString(headerBlockStyle.Render(header))
	b.WriteString("\n")

	b.WriteString(sectionTitleStyle.Render("Kickstart / payload"))
	b.WriteString("\n")

	for i, rom := range m.roms {
		line := "  " + rom.Name
		if i == m.cursor {
			line = "> " + rom.Name
			b.WriteString(selectedItemStyle.Render(line))
		} else {
			b.WriteString(itemStyle.Render(line))
		}
		b.WriteString("\n")
	}

	b.WriteString("\n")
	b.WriteString(sectionTitleStyle.Render("Options"))
	b.WriteString("\n")

	profileBadge := offBadgeStyle.Render("EMU68")
	if m.emuProfile == "bellatrix" {
		profileBadge = onBadgeStyle.Render("BELLATRIX")
	}
	b.WriteString(fmt.Sprintf("%s %s", itemStyle.Render("Emulator:"), profileBadge))
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
	b.WriteString("\n\n")

	b.WriteString(sectionTitleStyle.Render("QEMU command"))
	b.WriteString("\n")
	b.WriteString(commandStyle.Render(m.qemuCommand()))
	b.WriteString("\n")

	b.WriteString(helpStyle.Render("↑/↓ Navigate • E Toggle Emulator • D Toggle Display • B Toggle Debug • Enter Run • Q Quit"))

	return panelStyle.Render(b.String())
}

func (m model) qemuCommand() string {
	displayArg := "gtk,zoom-to-fit=on,window-close=on"
	if m.displayMode == "none" {
		displayArg = "none"
	}

	image := "emu68/install-bellatrix/Emu68.img"
	dtb := "emu68/install-bellatrix/bcm2710-rpi-3-b.dtb"
	if m.emuProfile == "emu68" {
		image = "emu68/build/Emu68.img"
		dtb = "emu68/build/firmware/bcm2710-rpi-3-b.dtb"
	}

	bootArgs := "enable_cache"
	if m.debugMode != "" {
		bootArgs = "enable_cache " + m.debugMode
	}

	base := fmt.Sprintf(
		`qemu-system-aarch64 -M raspi3b -kernel %s -dtb %s -serial stdio -display %s -append "%s"`,
		image,
		dtb,
		displayArg,
		bootArgs,
	)

	selected := m.roms[m.cursor]
	if selected.None {
		return base
	}

	return fmt.Sprintf("%s -initrd %s", base, selected.Path)
}
