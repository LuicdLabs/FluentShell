namespace FluentShell.Gui;

// The risk warning, shown before global apply is ever enabled.  Accepting requires typing the phrase, so the
// acceptance is a deliberate act and not a reflexive click.
internal sealed class ConsentForm : Form {
    internal const string RequiredPhrase = "I ACCEPT THE RISK";

    public ConsentForm(string warningText) {
        Text = "Global apply: risk warning";
        StartPosition = FormStartPosition.CenterParent;
        FormBorderStyle = FormBorderStyle.FixedDialog;
        MinimizeBox = false;
        MaximizeBox = false;
        ClientSize = new Size(640, 520);
        Padding = new Padding(12);

        var warning = new TextBox {
            Multiline = true,
            ReadOnly = true,
            ScrollBars = ScrollBars.Vertical,
            Text = warningText.Replace("\r\n", "\n").Replace("\n", Environment.NewLine),
            Dock = DockStyle.Top,
            Height = 360,
            BackColor = SystemColors.Window,
        };

        var prompt = new Label {
            Text = $"To enable global apply, type {RequiredPhrase} exactly:",
            AutoSize = true,
            Dock = DockStyle.Top,
            Padding = new Padding(0, 10, 0, 4),
        };

        var phrase = new TextBox { Dock = DockStyle.Top };

        var accept = new Button {
            Text = "Accept and enable",
            Enabled = false,
            DialogResult = DialogResult.OK,
            AutoSize = true,
        };
        var cancel = new Button {
            Text = "Cancel",
            DialogResult = DialogResult.Cancel,
            AutoSize = true,
        };

        phrase.TextChanged += (_, _) => accept.Enabled = phrase.Text == RequiredPhrase;

        var buttons = new FlowLayoutPanel {
            FlowDirection = FlowDirection.RightToLeft,
            Dock = DockStyle.Bottom,
            AutoSize = true,
            Padding = new Padding(0, 10, 0, 0),
        };
        buttons.Controls.Add(cancel);
        buttons.Controls.Add(accept);

        // WinForms docks the first control added first, so the order here is the visual order top to bottom.
        Controls.Add(warning);
        Controls.Add(prompt);
        Controls.Add(phrase);
        Controls.Add(buttons);

        AcceptButton = accept;
        CancelButton = cancel;
    }
}
