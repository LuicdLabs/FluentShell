using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Data;
using System.Text;

namespace FluentShell.Renderer.Windows;

internal sealed class BooleanToVisibilityConverter : IValueConverter
{
    public object Convert(object value, Type targetType, object parameter, string language) =>
        value is true ? Visibility.Visible : Visibility.Collapsed;

    public object ConvertBack(object value, Type targetType, object parameter, string language) =>
        value is Visibility.Visible;
}

internal sealed class Win32MnemonicTextConverter : IValueConverter
{
    public object Convert(object value, Type targetType, object parameter, string language) =>
        Win32Mnemonic.DisplayText(value as string ?? string.Empty);

    public object ConvertBack(object value, Type targetType, object parameter, string language) =>
        throw new NotSupportedException();
}

internal static class Win32Mnemonic
{
    public static string DisplayText(string value)
    {
        var result = new StringBuilder(value.Length);
        for (var index = 0; index < value.Length; index++)
        {
            if (value[index] != '&')
            {
                result.Append(value[index]);
                continue;
            }

            if (index + 1 < value.Length && value[index + 1] == '&')
            {
                result.Append('&');
                index++;
            }
            else if (index + 1 >= value.Length)
            {
                result.Append('&');
            }
        }
        return result.ToString();
    }

    public static string AccessKey(string value)
    {
        for (var index = 0; index + 1 < value.Length; index++)
        {
            if (value[index] != '&') continue;
            if (value[index + 1] == '&')
            {
                index++;
                continue;
            }
            return value[index + 1].ToString();
        }
        return string.Empty;
    }
}
