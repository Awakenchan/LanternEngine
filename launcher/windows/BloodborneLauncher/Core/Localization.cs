using System.Globalization;
using System.IO;
using System.Reflection;
using System.Text.Json;
using System.Windows.Markup;

namespace BloodborneLauncher.Core;

/// <summary>Presentation-only translations; configuration keys and values remain unchanged.</summary>
public static class Localization
{
    /// <summary>The launcher languages, as saved in ui_language, with their own names. Every one but English
    /// is Localization/&lt;code&gt;.json (English text to translation); the settings pages list them in this order.</summary>
    public static readonly IReadOnlyList<(string Code, string Name)> Languages = new[]
    {
        ("en", "English"), ("de", "Deutsch"), ("es", "Español"), ("fr", "Français"), ("it", "Italiano"),
        ("pl", "Polski"), ("pt-BR", "Português (Brasil)"), ("tr", "Türkçe"), ("ru", "Русский"), ("uk", "Українська"),
        ("ja", "日本語"), ("ko", "한국어"), ("zh-CN", "简体中文"), ("zh-TW", "繁體中文"),
    };

    private static IReadOnlyDictionary<string, string> _strings = new Dictionary<string, string>();
    public static string Language { get; private set; } = "en";

    /// <summary>A language's translations, or null for English and for codes not in <see cref="Languages"/>.</summary>
    public static IReadOnlyDictionary<string, string>? Catalog(string language)
    {
        if (language == "en" || !Languages.Any(l => l.Code == language)) return null;
        using var stream = Assembly.GetExecutingAssembly().GetManifestResourceStream($"BloodborneLauncher.Localization.{language}.json")
            ?? throw new InvalidOperationException($"Missing {language} translation resource.");
        using var doc = JsonDocument.Parse(stream);
        var strings = new Dictionary<string, string>();
        foreach (var entry in doc.RootElement.EnumerateObject())
            if (entry.Value.ValueKind == JsonValueKind.String)
                strings[entry.Name] = entry.Value.GetString()!;
        return strings;
    }

    /// <summary>Called before constructing the UI. English is the default, and the fallback for unknown
    /// languages and for any string a language file lacks.</summary>
    public static void Initialize(string language)
    {
        var catalog = Catalog(language);
        _strings = catalog ?? new Dictionary<string, string>();
        Language = catalog == null ? "en" : language;
    }

    public static void Initialize()
    {
        Initialize("en");
        try
        {
            using var settings = JsonDocument.Parse(File.ReadAllText(Paths.ConfigFile));
            if (settings.RootElement.ValueKind == JsonValueKind.Object &&
                settings.RootElement.TryGetProperty("ui_language", out var language) &&
                language.ValueKind == JsonValueKind.String)
                Initialize(language.GetString()!);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or JsonException)
        {
            // The regular settings loader handles missing or damaged files; do not migrate or save here.
        }
    }

    public static string T(string text) => _strings.TryGetValue(text, out var translated) ? translated : text;
    public static string F(FormattableString text) => string.Format(CultureInfo.CurrentCulture, T(text.Format), text.GetArguments());
}

#if AVALONIA
public sealed class TrExtension(string text) : Avalonia.Markup.Xaml.MarkupExtension
{
    public override object ProvideValue(IServiceProvider serviceProvider) => Localization.T(text);
}
#else
[MarkupExtensionReturnType(typeof(string))]
public sealed class TrExtension(string text) : MarkupExtension
{
    public override object ProvideValue(IServiceProvider serviceProvider) => Localization.T(text);
}
#endif
