using System.Globalization;
using System.Windows;

namespace WinAnanicyGui;

internal static class LocalizationService
{
    public static string CurrentLanguage { get; private set; } = "en";

    public static void Apply(string language)
    {
        language = language.Equals("tr", StringComparison.OrdinalIgnoreCase) ? "tr" : "en";
        var dictionaries = System.Windows.Application.Current.Resources.MergedDictionaries;
        var existing = dictionaries.FirstOrDefault(
            dictionary => dictionary.Source?.OriginalString.Contains(
                "Resources/Strings.",
                StringComparison.OrdinalIgnoreCase) == true);
        var replacement = new ResourceDictionary
        {
            Source = new Uri($"Resources/Strings.{language}.xaml", UriKind.Relative)
        };

        if (existing is null)
        {
            dictionaries.Insert(0, replacement);
        }
        else
        {
            var index = dictionaries.IndexOf(existing);
            dictionaries[index] = replacement;
        }

        CurrentLanguage = language;
        var culture = CultureInfo.GetCultureInfo(language == "tr" ? "tr-TR" : "en-US");
        CultureInfo.DefaultThreadCurrentCulture = culture;
        CultureInfo.DefaultThreadCurrentUICulture = culture;
    }

    public static string Text(string key)
    {
        return System.Windows.Application.Current.TryFindResource(key)?.ToString() ?? key;
    }
}
