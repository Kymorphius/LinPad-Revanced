import SwiftUI

/// Settings › SSH: whether the SSH server set up in Linux starts at every LinPad launch.
/// The same preference is iPad Settings › LinPad › Start SSH at Launch, and Linux reads it at
/// boot as /proc/ish/defaults/ssh_autostart (`linpad-ssh-autostart` in Linux changes it too).
struct SSHSettingsSection: View {
    static let autostartKey = "ssh.autostart"

    @Environment(\.desktopTheme) private var theme
    @AppStorage(SSHSettingsSection.autostartKey) private var autostart = true

    var body: some View {
        SettingsSection(title: "SSH", symbol: "terminal") {
            SettingsRow(title: "Start SSH at launch") {
                Toggle("Start SSH at launch", isOn: $autostart)
                    .labelsHidden()
                    .tint(theme.accent)
                    .accessibilityIdentifier("settings.ssh.autostart")
            }
            Text("Starts the SSH server installed by setup_guest_ssh.sh each time LinPad starts, also when LinPad opens in the terminal. Takes effect at the next launch.")
                .font(.caption)
                .foregroundStyle(theme.secondaryText)
                .fixedSize(horizontal: false, vertical: true)
        }
    }
}
