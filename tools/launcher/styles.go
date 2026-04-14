package main

import "github.com/charmbracelet/lipgloss"

var (
	appWidth = 76

	panelStyle = lipgloss.NewStyle().
			Width(appWidth).
			Border(lipgloss.RoundedBorder()).
			BorderForeground(lipgloss.Color("#5A5A78")).
			Padding(1, 2)

	headerBlockStyle = lipgloss.NewStyle().
				Width(appWidth - 4).
				Align(lipgloss.Center).
				MarginBottom(1)

	headerTitleStyle = lipgloss.NewStyle().
				Bold(true).
				Foreground(lipgloss.Color("#CBA6F7"))

	headerSubtitleStyle = lipgloss.NewStyle().
				Foreground(lipgloss.Color("#A6ADC8"))

	sectionTitleStyle = lipgloss.NewStyle().
				Bold(true).
				Foreground(lipgloss.Color("#89B4FA")).
				MarginTop(1)

	selectedItemStyle = lipgloss.NewStyle().
				Bold(true).
				Foreground(lipgloss.Color("#11111B")).
				Background(lipgloss.Color("#89B4FA")).
				Padding(0, 1)

	itemStyle = lipgloss.NewStyle().
			Foreground(lipgloss.Color("#CDD6F4"))

	mutedStyle = lipgloss.NewStyle().
			Foreground(lipgloss.Color("#7F849C"))

	commandStyle = lipgloss.NewStyle().
			Foreground(lipgloss.Color("#BAC2DE")).
			Background(lipgloss.Color("#181825")).
			Padding(0, 1)

	onBadgeStyle = lipgloss.NewStyle().
			Bold(true).
			Foreground(lipgloss.Color("#11111B")).
			Background(lipgloss.Color("#A6E3A1")).
			Padding(0, 1)

	offBadgeStyle = lipgloss.NewStyle().
			Bold(true).
			Foreground(lipgloss.Color("#11111B")).
			Background(lipgloss.Color("#F9E2AF")).
			Padding(0, 1)

	helpStyle = lipgloss.NewStyle().
			Width(appWidth - 4).
			Align(lipgloss.Center).
			Foreground(lipgloss.Color("#9399B2")).
			MarginTop(1)
)
