// bellatrix/tools/launcher/tui.go
package main

import (
	"fmt"
	"strings"

	tea "github.com/charmbracelet/bubbletea"
	"github.com/charmbracelet/lipgloss"
)

type launchResult struct {
	kickstart   string
	displayMode string
	cancelled   bool
}

type model struct {
	roms        []ROM
	cursor      int
	displayMode string
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

	return launchResult{
		kickstart:   kickstart,
		displayMode: fm.displayMode,
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

	b.WriteString(sectionTitleStyle.Render("Kickstart"))
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

	displayBadge := onBadgeStyle.Render("GTK")
	if m.displayMode == "none" {
		displayBadge = offBadgeStyle.Render("HEADLESS")
	}
	b.WriteString(fmt.Sprintf("%s %s", itemStyle.Render("Display:"), displayBadge))
	b.WriteString("\n\n")

	b.WriteString(sectionTitleStyle.Render("Preview"))
	b.WriteString("\n")
	b.WriteString(commandStyle.Render(m.previewCommand()))
	b.WriteString("\n")

	b.WriteString(helpStyle.Render("↑/↓ Navigate • D Toggle Display • Enter Run • Q Quit"))

	return panelStyle.Render(b.String())
}

func (m model) previewCommand() string {
	displayArg := "gtk,zoom-to-fit=on,window-close=on"
	if m.displayMode == "none" {
		displayArg = "none"
	}

	selected := m.roms[m.cursor]
	if selected.None {
		return fmt.Sprintf(
			"qemu-system-aarch64 -M raspi3b -kernel Emu68.img -dtb bcm2710-rpi-3-b.dtb -serial stdio -display %s -append console=ttyAMA0",
			displayArg,
		)
	}

	return fmt.Sprintf(
		"qemu-system-aarch64 -M raspi3b -kernel Emu68.img -dtb bcm2710-rpi-3-b.dtb -serial stdio -display %s -append console=ttyAMA0 -initrd %s",
		displayArg,
		selected.Path,
	)
}
