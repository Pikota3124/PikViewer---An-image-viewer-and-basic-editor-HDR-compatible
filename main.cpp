// PikViewer - visor/editor de imagenes HDR/WCG minimalista.
// Swap chain scRGB (FP16 lineal, 1.0 = 80 nits). Toda la UI se dibuja con Direct2D
// sobre esa misma swap chain y las anotaciones se pintan (Direct2D) directamente
// sobre una textura de la imagen, asi que DWM nunca aplica su EOTF.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#ifndef IDI_PIKVIEWER
#define IDI_PIKVIEWER 101
#endif
#ifndef IDI_PIKVIEWER_BLANK
#define IDI_PIKVIEWER_BLANK 102
#endif
#include <appmodel.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <d3d11.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <d2d1_1.h>
#include <d2d1_1helper.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cfloat>
#include <cstring>
#include <cwctype>
#include <initializer_list>
#include <system_error>
#include <utility>
#include <string>
#include <vector>
#include <filesystem>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "uuid.lib")

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;
typedef D2D1_POINT_2F P;

static const wchar_t* kExts[] = { L".jxr", L".wdp", L".png", L".jpg", L".jpeg", L".jfif", L".bmp",
                                  L".tif", L".tiff", L".gif", L".heic", L".heif", L".avif", L".webp" };
static const wchar_t* kOpenFilter =
    L"Imagenes\0*.jxr;*.wdp;*.png;*.jpg;*.jpeg;*.jfif;*.bmp;*.tif;*.tiff;*.gif;*.heic;*.heif;*.avif;*.webp\0\0";

// ------------------------------------------------------------------ shader
static const char* kShader = R"(
cbuffer CB : register(b0) {
    float2 scale; float2 offset;
    float sdrWhite; float isHdr; float eotf; float gamma;
    float brightness; float contrast; float saturation; float pad2;
    float2 uvMin; float2 uvMax;
};
Texture2D tex : register(t0);
SamplerState smp : register(s0);
struct VO { float4 pos : SV_Position; float2 uv : TEXCOORD0; };

VO VS(uint id : SV_VertexID) {
    VO o; float2 uv = float2(id & 1, id >> 1);
    o.uv = lerp(uvMin, uvMax, uv);
    o.pos = float4((uv * 2 - 1) * float2(1, -1) * scale + offset, 0, 1);
    return o;
}
float3 decode(float3 c) {
    c = max(c, 0);
    if (eotf < 0.5) {
        float3 lo = c / 12.92;
        float3 hi = pow((c + 0.055) / 1.055, 2.4);
        return lerp(lo, hi, step(0.04045, c));
    }
    return pow(c, eotf < 1.5 ? 2.2 : 2.4);
}
float4 PS(VO i) : SV_Target {
    float4 c = tex.Sample(smp, i.uv);
    // Gamma con comportamiento de "Niveles" de GIMP: actúa sobre los
    // medios tonos y mantiene negro/blanco en sus extremos. Para SDR lo
    // hacemos sobre los valores codificados antes de pasar a lineal, que
    // es mucho más parecido a cómo funciona Niveles en un editor de imagen.
    float3 src = max(c.rgb, 0.0);
    if (isHdr <= 0.5) {
        float levelsGamma = exp2(gamma); // -1..+1 -> 0.5..2.0, 1.0 = neutro
        src = pow(src, 1.0 / max(levelsGamma, 0.001));
    }
    float3 rgb = isHdr > 0.5 ? c.rgb : decode(src) * sdrWhite;
    float3 norm = max(rgb / max(sdrWhite, 1.0), 0.0);
    if (isHdr > 0.5) {
        float levelsGamma = exp2(gamma);
        norm = pow(norm, 1.0 / max(levelsGamma, 0.001));
    }
    // Brillo: desplaza los tonos medios sin levantar el negro ni quemar el blanco.
    // x + b*x*(1-x) mantiene 0 y 1 fijos y evita el efecto de capa blanquecina.
    norm = saturate(norm + brightness * norm * (1.0 - norm));
    // Contraste con recorrido útil pero progresivo: 1.0 = neutro.
    // El recorrido del slider es amplio para que pequeños movimientos no sean bruscos.
    float appliedContrast = 1.0 + contrast * 0.1;
    norm = (norm - 0.5) * appliedContrast + 0.5;
    float lum = dot(norm, float3(0.2126, 0.7152, 0.0722));
    float appliedSaturation = 1.0 + saturation;
    norm = lerp(float3(lum, lum, lum), norm, appliedSaturation);
    // Imagen SDR: un archivo SDR no puede superar el blanco SDR, asi que la vista previa se recorta igual
    // que lo que se guarda (en un monitor HDR, sin esto los ajustes "brillan" por encima del blanco y al
    // guardar esas luces se pierden).
    if (isHdr <= 0.5) norm = min(norm, 1.0);
    rgb = max(norm, 0.0) * max(sdrWhite, 1.0);
    return float4(rgb * c.a, 1);
}
)";

struct Cbuf { float scale[2]; float offset[2]; float sdrWhite; float isHdr; float eotf; float gamma; float brightness; float contrast; float saturation; float pad2; float uvMin[2]; float uvMax[2]; };

// ------------------------------------------------------------------ tipos
enum Tool { T_NONE = 0, T_PEN, T_HIGH, T_ERASE, T_RECT, T_ELL, T_LINE, T_ARROW, T_CROP, T_CROP_BOX };
enum BtnId { B_FILE = 1, B_PEN, B_HIGH, B_ERASE, B_SHAPE, B_CROP, B_CROP_BOX, B_UNDO, B_REDO, B_COLOR, B_WIDTH, B_INFO, B_SAVE, B_ADJUST };

struct Op { int type = 0; std::vector<P> pts; uint32_t color = 0; float width = 1; };
struct Button { int id; D2D1_RECT_F r; };
struct CR { float x, y, w, h; };
struct View { float dx, dy, dw, dh, f; CR c; };
struct Info { std::wstring container, pixfmt, cs; uint64_t fsize = 0; float maxN = 0, avgN = 0; };
struct MI { int id; const wchar_t* t; bool chk; };

struct App {
    HWND hwnd = nullptr;
    ComPtr<IWICImagingFactory> wic;
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<IDXGISwapChain3> sc;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11Buffer> cb;
    ComPtr<ID3D11SamplerState> smp;
    ComPtr<ID3D11RasterizerState> rast;
    // Direct2D / DirectWrite
    ComPtr<ID2D1Factory1> d2dF;
    ComPtr<ID2D1Device> d2dDev;
    ComPtr<ID2D1DeviceContext> d2d;
    ComPtr<ID2D1Bitmap1> bbBmp, workBmp;
    ComPtr<ID2D1SolidColorBrush> brush;
    ComPtr<ID2D1StrokeStyle> ssRound, ssSquare;
    ComPtr<IDWriteFactory> dw;
    ComPtr<IDWriteTextFormat> fCenter, fLeft, fWrap, fIcon;
    // imagen: base (original), work (base + anotaciones), disp (con mips)
    ComPtr<ID3D11Texture2D> base, work, disp;
    ComPtr<ID3D11ShaderResourceView> srv;
    UINT imgW = 0, imgH = 0;
    bool hdr = false, deep = false, canEdit = false;
    Info info;
    // GIF animado: fotogramas ya compuestos (lienzo completo RGBA8) y retardo de cada uno en ms
    std::vector<std::vector<BYTE>> gifFrames;
    std::vector<UINT> gifDelay;
    size_t gifIdx = 0;
    ID3D11Texture2D* gifBase = nullptr; // textura base con la que se cargó; si cambia, la animación ya no vale
    float white = 2.5f;
    int eotf = 0; // 0 sRGB por tramos, 1 gamma 2.2, 2 gamma 2.4
    // archivos
    std::vector<fs::path> files;
    int idx = 0;
    // UI
    std::vector<Button> btns;
    std::vector<float> seps;
    int hoverBtn = 0, hover = 0;
    bool showInfo = false, full = false, showTop = false, showAdjust = false;
    int adjustDrag = -1;
    float gamma = 0.0f, brightness = 0.0f, contrast = 0.0f, saturation = 0.0f;
    // Redimensionado integrado en Ajustes
    int resizeW = 0, resizeH = 0;
    bool resizeDirty = false, keepAspect = true;
    int resizeField = 0;
    bool resizeSelectAll = false;
    std::wstring resizeWText, resizeHText;
    float resizeAspect = 1.0f;
    bool defaultPrompt = false;
    int dialog = 0; // 0 ninguno, 1 sobrescritura, 2 cambios sin guardar
    int dialogResult = 0; // 1 = accion principal, 2 = no guardar/no, 3 = cancelar
    WINDOWPLACEMENT prev{ sizeof(WINDOWPLACEMENT) };
    // edicion
    int tool = T_NONE, shape = T_RECT;
    uint32_t color = 0xFF3B30;
    int widthIdx = 1;
    std::vector<Op> ops;
    bool adjustDirty = false;
    // Historial: cada entrada guarda ops y, si hubo redimensionado, un snapshot de la imagen.
    struct Hist {
        std::vector<Op> ops;
        bool hasImg = false;
        UINT imgW = 0, imgH = 0;
        ComPtr<ID3D11Texture2D> baseSnap;
        float gamma = 0, brightness = 0, contrast = 0, saturation = 0;
        bool resizeDirty = false;
        int resizeW = 0, resizeH = 0;
        bool hdr = false, deep = false; // formato de la imagen del snapshot (puede cambiar al guardar HDR -> SDR)
        Info info;
    };
    std::vector<Hist> undoS, redoS;
    Op cur; bool drawing = false, mouseDown = false, erased = false;
    bool cropping = false; P cropA{}, cropB{}; int cropHandle = 0; bool cropBoxChanged = false;
    int mx = 0, my = 0;
    // zoom / desplazamiento de la vista (coordenadas de imagen)
    float zoom = 1.0f;
    float panX = 0.0f, panY = 0.0f;
    bool panning = false;
    int panStartX = 0, panStartY = 0;
    float panOrigX = 0.0f, panOrigY = 0.0f;
} g;

static const float kWidths[] = { 2, 4, 8, 14 };
struct ColorDef { const wchar_t* name; uint32_t rgb; };
static const ColorDef kColors[] = { { L"Rojo", 0xFF3B30 }, { L"Naranja", 0xFF9500 }, { L"Amarillo", 0xFFE600 },
    { L"Verde", 0x34C759 }, { L"Azul", 0x0A84FF }, { L"Blanco", 0xFFFFFF }, { L"Negro", 0x000000 } };


enum class UiLang { EN, ES, FR, DE, IT, PT, NL, PL, RU, JA, KO, ZH_CN, ZH_TW, TR };
static UiLang GetUiLang() {
    wchar_t name[LOCALE_NAME_MAX_LENGTH]{};
    GetUserDefaultLocaleName(name, LOCALE_NAME_MAX_LENGTH);
    std::wstring n(name);
    for (auto& c : n) c = (wchar_t)towlower(c);
    if (n.rfind(L"es",0)==0) return UiLang::ES;
    if (n.rfind(L"fr",0)==0) return UiLang::FR;
    if (n.rfind(L"de",0)==0) return UiLang::DE;
    if (n.rfind(L"it",0)==0) return UiLang::IT;
    if (n.rfind(L"pt-br",0)==0) return UiLang::PT;
    if (n.rfind(L"pt",0)==0) return UiLang::PT;
    if (n.rfind(L"nl",0)==0) return UiLang::NL;
    if (n.rfind(L"pl",0)==0) return UiLang::PL;
    if (n.rfind(L"ru",0)==0) return UiLang::RU;
    if (n.rfind(L"ja",0)==0) return UiLang::JA;
    if (n.rfind(L"ko",0)==0) return UiLang::KO;
    if (n.rfind(L"zh-tw",0)==0 || n.rfind(L"zh-hk",0)==0 || n.rfind(L"zh-mo",0)==0) return UiLang::ZH_TW;
    if (n.rfind(L"zh",0)==0) return UiLang::ZH_CN;
    if (n.rfind(L"tr",0)==0) return UiLang::TR;
    return UiLang::EN;
}
static const wchar_t* UiLocaleName() {
    static wchar_t name[LOCALE_NAME_MAX_LENGTH]{};
    static bool init = false;
    if (!init) {
        if (GetUserDefaultLocaleName(name, LOCALE_NAME_MAX_LENGTH) == 0) wcscpy_s(name, L"en-US");
        init = true;
    }
    return name;
}
static const wchar_t* T(const wchar_t* s) {
    UiLang l=GetUiLang();
    if (wcscmp(s,L"Archivo")==0) { static const wchar_t* a[]={L"File",L"Archivo",L"Fichier",L"Datei",L"File",L"Arquivo",L"Bestand",L"Plik",L"Файл",L"ファイル",L"파일",L"文件",L"檔案",L"Dosya"}; return a[(int)l]; }
    if (wcscmp(s,L"Ajustes")==0) { static const wchar_t* a[]={L"Settings",L"Ajustes",L"Réglages",L"Einstellungen",L"Impostazioni",L"Definições",L"Instellingen",L"Ustawienia",L"Настройки",L"設定",L"설정",L"设置",L"設定",L"Ayarlar"}; return a[(int)l]; }
    if (wcscmp(s,L"Abrir…")==0) { static const wchar_t* a[]={L"Open…",L"Abrir…",L"Ouvrir…",L"Öffnen…",L"Apri…",L"Abrir…",L"Openen…",L"Otwórz…",L"Открыть…",L"開く…",L"열기…",L"打开…",L"開啟…",L"Aç…"}; return a[(int)l]; }
    if (wcscmp(s,L"Guardar")==0) { static const wchar_t* a[]={L"Save",L"Guardar",L"Enregistrer",L"Speichern",L"Salva",L"Guardar",L"Opslaan",L"Zapisz",L"Сохранить",L"保存",L"저장",L"保存",L"儲存",L"Kaydet"}; return a[(int)l]; }
    if (wcscmp(s,L"Guardar como…")==0) { static const wchar_t* a[]={L"Save as…",L"Guardar como…",L"Enregistrer sous…",L"Speichern unter…",L"Salva con nome…",L"Guardar como…",L"Opslaan als…",L"Zapisz jako…",L"Сохранить как…",L"名前を付けて保存…",L"다른 이름으로 저장…",L"另存为…",L"另存新檔…",L"Farklı kaydet…"}; return a[(int)l]; }
    if (wcscmp(s,L"Salir")==0) { static const wchar_t* a[]={L"Exit",L"Salir",L"Quitter",L"Beenden",L"Esci",L"Sair",L"Afsluiten",L"Wyjdź",L"Выход",L"終了",L"종료",L"退出",L"結束",L"Çıkış"}; return a[(int)l]; }
    if (wcscmp(s,L"Rectángulo")==0) { static const wchar_t* a[]={L"Rectangle",L"Rectángulo",L"Rectangle",L"Rechteck",L"Rettangolo",L"Retângulo",L"Rechthoek",L"Prostokąt",L"Прямоугольник",L"長方形",L"사각형",L"矩形",L"矩形",L"Dikdörtgen"}; return a[(int)l]; }
    if (wcscmp(s,L"Elipse")==0) { static const wchar_t* a[]={L"Ellipse",L"Elipse",L"Ellipse",L"Ellipse",L"Ellisse",L"Elipse",L"Ellips",L"Elipsa",L"Эллипс",L"楕円",L"타원",L"椭圆",L"橢圓",L"Elips"}; return a[(int)l]; }
    if (wcscmp(s,L"Línea")==0) { static const wchar_t* a[]={L"Line",L"Línea",L"Ligne",L"Linie",L"Linea",L"Linha",L"Lijn",L"Linia",L"Линия",L"線",L"선",L"直线",L"直線",L"Çizgi"}; return a[(int)l]; }
    if (wcscmp(s,L"Flecha")==0) { static const wchar_t* a[]={L"Arrow",L"Flecha",L"Flèche",L"Pfeil",L"Freccia",L"Seta",L"Pijl",L"Strzałka",L"Стрелка",L"矢印",L"화살표",L"箭头",L"箭頭",L"Ok"}; return a[(int)l]; }
    if (wcscmp(s,L"Fino")==0) { static const wchar_t* a[]={L"Thin",L"Fino",L"Fin",L"Dünn",L"Sottile",L"Fino",L"Dun",L"Cienkie",L"Тонкий",L"細い",L"얇게",L"细",L"細",L"İnce"}; return a[(int)l]; }
    if (wcscmp(s,L"Medio")==0) { static const wchar_t* a[]={L"Medium",L"Medio",L"Moyen",L"Mittel",L"Medio",L"Médio",L"Gemiddeld",L"Średnie",L"Средний",L"中",L"중간",L"中",L"中",L"Orta"}; return a[(int)l]; }
    if (wcscmp(s,L"Grueso")==0) { static const wchar_t* a[]={L"Thick",L"Grueso",L"Épais",L"Dick",L"Spesso",L"Grosso",L"Dik",L"Grube",L"Толстый",L"太い",L"굵게",L"粗",L"粗",L"Kalın"}; return a[(int)l]; }
    if (wcscmp(s,L"Muy grueso")==0) { static const wchar_t* a[]={L"Very thick",L"Muy grueso",L"Très épais",L"Sehr dick",L"Molto spesso",L"Muito grosso",L"Zeer dik",L"Bardzo grube",L"Очень толстый",L"極太",L"매우 굵게",L"很粗",L"很粗",L"Çok kalın"}; return a[(int)l]; }
    if (wcscmp(s,L"Rojo")==0) { static const wchar_t* a[]={L"Red",L"Rojo",L"Rouge",L"Rot",L"Rosso",L"Vermelho",L"Rood",L"Czerwony",L"Красный",L"赤",L"빨강",L"红色",L"紅色",L"Kırmızı"}; return a[(int)l]; }
    if (wcscmp(s,L"Naranja")==0) { static const wchar_t* a[]={L"Orange",L"Naranja",L"Orange",L"Orange",L"Arancione",L"Laranja",L"Oranje",L"Pomarańczowy",L"Оранжевый",L"オレンジ",L"주황",L"橙色",L"橙色",L"Turuncu"}; return a[(int)l]; }
    if (wcscmp(s,L"Amarillo")==0) { static const wchar_t* a[]={L"Yellow",L"Amarillo",L"Jaune",L"Gelb",L"Giallo",L"Amarelo",L"Geel",L"Żółty",L"Жёлтый",L"黄色",L"노랑",L"黄色",L"黃色",L"Sarı"}; return a[(int)l]; }
    if (wcscmp(s,L"Verde")==0) { static const wchar_t* a[]={L"Green",L"Verde",L"Vert",L"Grün",L"Verde",L"Verde",L"Groen",L"Zielony",L"Зелёный",L"緑",L"초록",L"绿色",L"綠色",L"Yeşil"}; return a[(int)l]; }
    if (wcscmp(s,L"Azul")==0) { static const wchar_t* a[]={L"Blue",L"Azul",L"Bleu",L"Blau",L"Blu",L"Azul",L"Blauw",L"Niebieski",L"Синий",L"青",L"파랑",L"蓝色",L"藍色",L"Mavi"}; return a[(int)l]; }
    if (wcscmp(s,L"Blanco")==0) { static const wchar_t* a[]={L"White",L"Blanco",L"Blanc",L"Weiß",L"Bianco",L"Branco",L"Wit",L"Biały",L"Белый",L"白",L"흰색",L"白色",L"白色",L"Beyaz"}; return a[(int)l]; }
    if (wcscmp(s,L"Negro")==0) { static const wchar_t* a[]={L"Black",L"Negro",L"Noir",L"Schwarz",L"Nero",L"Preto",L"Zwart",L"Czarny",L"Чёрный",L"黒",L"검정",L"黑色",L"黑色",L"Siyah"}; return a[(int)l]; }
    if (wcscmp(s,L"Sí")==0) { static const wchar_t* a[]={L"Yes",L"Sí",L"Oui",L"Ja",L"Sì",L"Sim",L"Ja",L"Tak",L"Да",L"はい",L"예",L"是",L"是",L"Evet"}; return a[(int)l]; }
    if (wcscmp(s,L"No")==0) { static const wchar_t* a[]={L"No",L"No",L"Non",L"Nein",L"No",L"Não",L"Nee",L"Nie",L"Нет",L"いいえ",L"아니요",L"否",L"否",L"Hayır"}; return a[(int)l]; }
    if (wcscmp(s,L"Cancelar")==0) { static const wchar_t* a[]={L"Cancel",L"Cancelar",L"Annuler",L"Abbrechen",L"Annulla",L"Cancelar",L"Annuleren",L"Anuluj",L"Отмена",L"キャンセル",L"취소",L"取消",L"取消",L"İptal"}; return a[(int)l]; }
    if (wcscmp(s,L"Guardar imagen")==0) { static const wchar_t* a[]={L"Save image",L"Guardar imagen",L"Enregistrer l’image",L"Bild speichern",L"Salva immagine",L"Guardar imagem",L"Afbeelding opslaan",L"Zapisz obraz",L"Сохранить изображение",L"画像を保存",L"이미지 저장",L"保存图像",L"儲存圖片",L"Resmi kaydet"}; return a[(int)l]; }
    if (wcscmp(s,L"Cambios sin guardar")==0) { static const wchar_t* a[]={L"Unsaved changes",L"Cambios sin guardar",L"Modifications non enregistrées",L"Nicht gespeicherte Änderungen",L"Modifiche non salvate",L"Alterações não guardadas",L"Niet-opgeslagen wijzigingen",L"Niezapisane zmiany",L"Несохранённые изменения",L"未保存の変更",L"저장되지 않은 변경 사항",L"未保存的更改",L"未儲存的變更",L"Kaydedilmemiş değişiklikler"}; return a[(int)l]; }
    if (wcscmp(s,L"¿Quieres usar PikViewer como visor predeterminado?")==0) { static const wchar_t* a[]={L"Do you want to use PikViewer as the default viewer?",L"¿Quieres usar PikViewer como visor predeterminado?",L"Voulez-vous utiliser PikViewer comme visionneuse par défaut ?",L"Möchten Sie PikViewer als Standardanzeige verwenden?",L"Vuoi usare PikViewer come visualizzatore predefinito?",L"Quer utilizar o PikViewer como visualizador predefinido?",L"Wilt u PikViewer als standaardviewer gebruiken?",L"Czy chcesz używać PikViewer jako domyślnej przeglądarki?",L"Использовать PikViewer как средство просмотра по умолчанию?",L"PikViewerを既定のビューアーとして使用しますか？",L"PikViewer를 기본 뷰어로 사용하시겠습니까?",L"是否要将 PikViewer 用作默认查看器？",L"是否要將 PikViewer 設為預設檢視器？",L"PikViewer varsayılan görüntüleyici olarak kullanılsın mı?"}; return a[(int)l]; }
    if (wcscmp(s,L"Posición")==0) { static const wchar_t* a[]={L"Position",L"Posición",L"Position",L"Position",L"Posizione",L"Posição",L"Positie",L"Pozycja",L"Позиция",L"位置",L"위치",L"位置",L"位置",L"Konum"}; return a[(int)l]; }
    if (wcscmp(s,L"Dimensiones")==0) { static const wchar_t* a[]={L"Dimensions",L"Dimensiones",L"Dimensions",L"Abmessungen",L"Dimensioni",L"Dimensões",L"Afmetingen",L"Wymiary",L"Размеры",L"サイズ",L"크기",L"尺寸",L"尺寸",L"Boyutlar"}; return a[(int)l]; }
    if (wcscmp(s,L"Recorte")==0) { static const wchar_t* a[]={L"Crop",L"Recorte",L"Recadrage",L"Zuschnitt",L"Ritaglio",L"Corte",L"Bijsnijden",L"Przycięcie",L"Обрезка",L"トリミング",L"자르기",L"裁剪",L"裁切",L"Kırpma"}; return a[(int)l]; }
    if (wcscmp(s,L"Tamaño")==0) { static const wchar_t* a[]={L"Size",L"Tamaño",L"Taille",L"Größe",L"Dimensione",L"Tamanho",L"Grootte",L"Rozmiar",L"Размер",L"サイズ",L"크기",L"大小",L"大小",L"Boyut"}; return a[(int)l]; }
    if (wcscmp(s,L"Formato")==0) { static const wchar_t* a[]={L"Format",L"Formato",L"Format",L"Format",L"Formato",L"Formato",L"Formaat",L"Format",L"Формат",L"形式",L"형식",L"格式",L"格式",L"Biçim"}; return a[(int)l]; }
    if (wcscmp(s,L"Píxel")==0) { static const wchar_t* a[]={L"Pixel",L"Píxel",L"Pixel",L"Pixel",L"Pixel",L"Pixel",L"Pixel",L"Piksel",L"Пиксель",L"ピクセル",L"픽셀",L"像素",L"像素",L"Piksel"}; return a[(int)l]; }
    if (wcscmp(s,L"Rango")==0) { static const wchar_t* a[]={L"Range",L"Rango",L"Plage",L"Bereich",L"Gamma",L"Gama",L"Bereik",L"Zakres",L"Диапазон",L"レンジ",L"범위",L"范围",L"範圍",L"Aralık"}; return a[(int)l]; }
    if (wcscmp(s,L"Espacio de color")==0) { static const wchar_t* a[]={L"Color space",L"Espacio de color",L"Espace colorimétrique",L"Farbraum",L"Spazio colore",L"Espaço de cor",L"Kleurruimte",L"Przestrzeń kolorów",L"Цветовое пространство",L"色空間",L"색 공간",L"色彩空间",L"色彩空間",L"Renk alanı"}; return a[(int)l]; }
    if (wcscmp(s,L"Brillo máximo")==0) { static const wchar_t* a[]={L"Peak brightness",L"Brillo máximo",L"Luminosité maximale",L"Maximale Helligkeit",L"Luminosità massima",L"Brilho máximo",L"Maximale helderheid",L"Maksymalna jasność",L"Максимальная яркость",L"最大輝度",L"최대 밝기",L"峰值亮度",L"峰值亮度",L"Maksimum parlaklık"}; return a[(int)l]; }
    if (wcscmp(s,L"Brillo medio")==0) { static const wchar_t* a[]={L"Average brightness",L"Brillo medio",L"Luminosité moyenne",L"Durchschnittliche Helligkeit",L"Luminosità media",L"Brilho médio",L"Gemiddelde helderheid",L"Średnia jasność",L"Средняя яркость",L"平均輝度",L"평균 밝기",L"平均亮度",L"平均亮度",L"Ortalama parlaklık"}; return a[(int)l]; }
    if (wcscmp(s,L"Curva (tecla E)")==0) { static const wchar_t* a[]={L"Curve (E key)",L"Curva (tecla E)",L"Courbe (touche E)",L"Kurve (E-Taste)",L"Curva (tasto E)",L"Curva (tecla E)",L"Curve (E-toets)",L"Krzywa (klawisz E)",L"Кривая (клавиша E)",L"カーブ (Eキー)",L"곡선 (E 키)",L"曲线（E 键）",L"曲線（E 鍵）",L"E tuşu eğrisi"}; return a[(int)l]; }
    if (wcscmp(s,L"Blanco SDR del sistema")==0) { static const wchar_t* a[]={L"System SDR white",L"Blanco SDR del sistema",L"Blanc SDR du système",L"System-SDR-Weiß",L"Bianco SDR di sistema",L"Branco SDR do sistema",L"Systeem-SDR-wit",L"Biel SDR systemu",L"Белый SDR системы",L"システムSDR白色",L"시스템 SDR 흰색",L"系统 SDR 白点",L"系統 SDR 白點",L"Sistem SDR beyazı"}; return a[(int)l]; }
    if (wcscmp(s,L"Ajustes de imagen")==0) { static const wchar_t* a[]={L"Image adjustments",L"Ajustes de imagen",L"Réglages de l’image",L"Bildeinstellungen",L"Regolazioni immagine",L"Ajustes de imagem",L"Beeldinstellingen",L"Korekty obrazu",L"Настройки изображения",L"画像調整",L"이미지 조정",L"图像调整",L"影像調整",L"Görüntü ayarları"}; return a[(int)l]; }
    if (wcscmp(s,L"Ancho")==0) { static const wchar_t* a[]={L"Width",L"Ancho",L"Largeur",L"Breite",L"Larghezza",L"Largura",L"Breedte",L"Szerokość",L"Ширина",L"幅",L"너비",L"宽度",L"寬度",L"Genişlik"}; return a[(int)l]; }
    if (wcscmp(s,L"Alto")==0) { static const wchar_t* a[]={L"Height",L"Alto",L"Hauteur",L"Höhe",L"Altezza",L"Altura",L"Hoogte",L"Wysokość",L"Высота",L"高さ",L"높이",L"高度",L"高度",L"Yükseklik"}; return a[(int)l]; }
    if (wcscmp(s,L"Mantener proporción")==0) { static const wchar_t* a[]={L"Keep aspect ratio",L"Mantener proporción",L"Conserver les proportions",L"Seitenverhältnis beibehalten",L"Mantieni proporzioni",L"Manter proporção",L"Beeldverhouding behouden",L"Zachowaj proporcje",L"Сохранять пропорции",L"縦横比を維持",L"비율 유지",L"保持宽高比",L"保持寬高比",L"En-boy oranını koru"}; return a[(int)l]; }
    if (wcscmp(s,L"Aplicar tamaño")==0) { static const wchar_t* a[]={L"Apply size",L"Aplicar tamaño",L"Appliquer la taille",L"Größe anwenden",L"Applica dimensioni",L"Aplicar tamanho",L"Grootte toepassen",L"Zastosuj rozmiar",L"Применить размер",L"サイズを適用",L"크기 적용",L"应用大小",L"套用大小",L"Boyutu uygula"}; return a[(int)l]; }
    if (wcscmp(s,L"Brillo")==0) { static const wchar_t* a[]={L"Brightness",L"Brillo",L"Luminosité",L"Helligkeit",L"Luminosità",L"Brilho",L"Helderheid",L"Jasność",L"Яркость",L"明るさ",L"밝기",L"亮度",L"亮度",L"Parlaklık"}; return a[(int)l]; }
    if (wcscmp(s,L"Contraste")==0) { static const wchar_t* a[]={L"Contrast",L"Contraste",L"Contraste",L"Kontrast",L"Contrasto",L"Contraste",L"Contrast",L"Kontrast",L"Контраст",L"コントラスト",L"대비",L"对比度",L"對比度",L"Kontrast"}; return a[(int)l]; }
    if (wcscmp(s,L"Saturación")==0) { static const wchar_t* a[]={L"Saturation",L"Saturación",L"Saturation",L"Sättigung",L"Saturazione",L"Saturação",L"Verzadiging",L"Nasycenie",L"Насыщенность",L"彩度",L"채도",L"饱和度",L"飽和度",L"Doygunluk"}; return a[(int)l]; }
    if (wcscmp(s,L"No guardar")==0) { static const wchar_t* a[]={L"Don't save",L"No guardar",L"Ne pas enregistrer",L"Nicht speichern",L"Non salvare",L"Não guardar",L"Niet opslaan",L"Nie zapisuj",L"Не сохранять",L"保存しない",L"저장 안 함",L"不保存",L"不儲存",L"Kaydetme"}; return a[(int)l]; }
    if (wcscmp(s,L"Se sobrescribirá el archivo original. ¿Continuar?")==0) { static const wchar_t* a[]={L"The original file will be overwritten. Continue?",L"Se sobrescribirá el archivo original. ¿Continuar?",L"Le fichier original sera remplacé. Continuer ?",L"Die Originaldatei wird überschrieben. Fortfahren?",L"Il file originale verrà sovrascritto. Continuare?",L"O ficheiro original será substituído. Continuar?",L"Het originele bestand wordt overschreven. Doorgaan?",L"Oryginalny plik zostanie nadpisany. Kontynuować?",L"Исходный файл будет перезаписан. Продолжить?",L"元のファイルを上書きします。続行しますか？",L"원본 파일을 덮어씁니다. 계속하시겠습니까?",L"将覆盖原始文件。继续吗？",L"將覆寫原始檔案。繼續嗎？",L"Orijinal dosyanın üzerine yazılacak. Devam edilsin mi?"}; return a[(int)l]; }
    if (wcscmp(s,L"Hay cambios sin guardar. ¿Qué quieres hacer?")==0) { static const wchar_t* a[]={L"There are unsaved changes. What do you want to do?",L"Hay cambios sin guardar. ¿Qué quieres hacer?",L"Des modifications ne sont pas enregistrées. Que voulez-vous faire ?",L"Es gibt nicht gespeicherte Änderungen. Was möchten Sie tun?",L"Ci sono modifiche non salvate. Cosa vuoi fare?",L"Existem alterações não guardadas. O que pretende fazer?",L"Er zijn niet-opgeslagen wijzigingen. Wat wilt u doen?",L"Istnieją niezapisane zmiany. Co chcesz zrobić?",L"Есть несохранённые изменения. Что сделать?",L"未保存の変更があります。どうしますか？",L"저장되지 않은 변경 사항이 있습니다. 어떻게 하시겠습니까?",L"有未保存的更改。要怎么做？",L"有未儲存的變更。要怎麼做？",L"Kaydedilmemiş değişiklikler var. Ne yapmak istiyorsunuz?"}; return a[(int)l]; }
    if (wcscmp(s,L"No se pudo escribir el archivo.")==0) { static const wchar_t* a[]={L"Could not write the file.",L"No se pudo escribir el archivo.",L"Impossible d’écrire le fichier.",L"Die Datei konnte nicht geschrieben werden.",L"Impossibile scrivere il file.",L"Não foi possível escrever o ficheiro.",L"Kan het bestand niet schrijven.",L"Nie można zapisać pliku.",L"Не удалось записать файл.",L"ファイルを書き込めませんでした。",L"파일을 쓸 수 없습니다.",L"无法写入文件。",L"無法寫入檔案。",L"Dosya yazılamadı."}; return a[(int)l]; }
    if (wcscmp(s,L"No se pudo abrir la imagen.")==0) { static const wchar_t* a[]={L"Could not open the image.",L"No se pudo abrir la imagen.",L"Impossible d’ouvrir l’image.",L"Das Bild konnte nicht geöffnet werden.",L"Impossibile aprire l’immagine.",L"Não foi possível abrir a imagem.",L"Kan de afbeelding niet openen.",L"Nie można otworzyć obrazu.",L"Не удалось открыть изображение.",L"画像を開けませんでした。",L"이미지를 열 수 없습니다.",L"无法打开图像。",L"無法開啟圖片。",L"Görüntü açılamadı."}; return a[(int)l]; }
    if (wcscmp(s,L"No se pudo inicializar Direct3D 11 / Direct2D.")==0) { static const wchar_t* a[]={L"Could not initialize Direct3D 11 / Direct2D.",L"No se pudo inicializar Direct3D 11 / Direct2D.",L"Impossible d’initialiser Direct3D 11 / Direct2D.",L"Direct3D 11 / Direct2D konnte nicht initialisiert werden.",L"Impossibile inizializzare Direct3D 11 / Direct2D.",L"Não foi possível inicializar o Direct3D 11 / Direct2D.",L"Kan Direct3D 11 / Direct2D niet initialiseren.",L"Nie można zainicjować Direct3D 11 / Direct2D.",L"Не удалось инициализировать Direct3D 11 / Direct2D.",L"Direct3D 11 / Direct2Dを初期化できませんでした。",L"Direct3D 11 / Direct2D를 초기화할 수 없습니다.",L"无法初始化 Direct3D 11 / Direct2D。",L"無法初始化 Direct3D 11 / Direct2D。",L"Direct3D 11 / Direct2D başlatılamadı."}; return a[(int)l]; }
    if (wcscmp(s,L"Formato de salida no soportado.")==0) { static const wchar_t* a[]={L"Unsupported output format.",L"Formato de salida no soportado.",L"Format de sortie non pris en charge.",L"Nicht unterstütztes Ausgabeformat.",L"Formato di output non supportato.",L"Formato de saída não suportado.",L"Niet-ondersteund uitvoerformaat.",L"Nieobsługiwany format wyjściowy.",L"Неподдерживаемый формат вывода.",L"サポートされていない出力形式です。",L"지원되지 않는 출력 형식입니다.",L"不支持的输出格式。",L"不支援的輸出格式。",L"Desteklenmeyen çıktı biçimi."}; return a[(int)l]; }
    if (wcscmp(s,L"Se abrirá la configuración de Windows para que puedas establecer PikViewer como\npredeterminado para sus formatos de imagen compatibles.")==0) { static const wchar_t* a[]={L"Windows Settings will open so you can set PikViewer as\nthe default app for its supported image formats.",L"Se abrirá la configuración de Windows para que puedas establecer PikViewer como\npredeterminado para sus formatos de imagen compatibles.",L"Les paramètres Windows vont s’ouvrir pour définir PikViewer comme\napplication par défaut pour ses formats d’image pris en charge.",L"Die Windows-Einstellungen werden geöffnet, damit Sie PikViewer als\nStandard-App für unterstützte Bildformate festlegen können.",L"Le impostazioni di Windows verranno aperte per impostare PikViewer come\napp predefinita per i formati immagine supportati.",L"As Definições do Windows serão abertas para definir o PikViewer como\naplicação predefinida para os formatos de imagem suportados.",L"Windows-instellingen worden geopend zodat u PikViewer kunt instellen als\nde standaardapp voor de ondersteunde afbeeldingsindelingen.",L"Zostaną otwarte ustawienia systemu Windows, aby ustawić PikViewer jako\ndomyślną aplikację dla obsługiwanych formatów obrazów.",L"Откроются параметры Windows, где можно назначить PikViewer\nприложением по умолчанию для поддерживаемых форматов изображений.",L"Windowsの設定を開き、対応する画像形式の既定のアプリとして\nPikViewerを設定できます。",L"Windows 설정이 열리고 지원되는 이미지 형식의\n기본 앱으로 PikViewer를 설정할 수 있습니다.",L"Windows 设置将打开，你可以将 PikViewer 设置为\n支持的图像格式的默认应用。",L"Windows 設定會開啟，你可以將 PikViewer 設為\n支援影像格式的預設應用程式。",L"Windows Ayarları açılarak desteklenen görüntü biçimleri için\nPikViewer varsayılan uygulama olarak ayarlanabilir."}; return a[(int)l]; }
    if (wcscmp(s,L"sRGB por tramos")==0) { static const wchar_t* a[]={L"Piecewise sRGB",L"sRGB por tramos",L"sRGB par morceaux",L"sRGB stückweise",L"sRGB a tratti",L"sRGB por segmentos",L"sRGB per segmenten",L"sRGB odcinkowo",L"Ступенчатый sRGB",L"sRGB（区分）",L"구간별 sRGB",L"分段 sRGB",L"分段 sRGB",L"Parçalı sRGB"}; return a[(int)l]; }
    return s;
}

static float Dpi() { return GetDpiForWindow(g.hwnd) / 96.0f; }
static bool In(const D2D1_RECT_F& r, int x, int y) { return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom; }
static void Render();

static bool ShouldShowDefaultPrompt() {
    DWORD v = 0, cb = sizeof(v);
    return RegGetValueW(HKEY_CURRENT_USER, L"Software\\PikViewer", L"DefaultPromptDoneV2", RRF_RT_REG_DWORD, nullptr, &v, &cb) != ERROR_SUCCESS;
}

static void MarkDefaultPromptDone() {
    DWORD v = 1;
    RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\PikViewer", L"DefaultPromptDoneV2", REG_DWORD, &v, sizeof(v));
}

static std::wstring PackageFamilyName() {
    UINT32 len = 0;
    UINT32 count = 0;
    LONG rc = GetCurrentPackageInfo(0, &len, nullptr, &count);
    if (rc != ERROR_INSUFFICIENT_BUFFER || len == 0 || count == 0) return L"";
    std::vector<BYTE> buf(len);
    auto* pi = reinterpret_cast<PACKAGE_INFO*>(buf.data());
    UINT32 bufLen = len;
    if (GetCurrentPackageInfo(0, &bufLen, buf.data(), &count) != ERROR_SUCCESS || count == 0) return L"";
    UINT32 n = 0;
    if (PackageFamilyNameFromId(&pi[0].packageId, &n, nullptr) != ERROR_INSUFFICIENT_BUFFER || n == 0) return L"";
    std::vector<wchar_t> name(n);
    if (PackageFamilyNameFromId(&pi[0].packageId, &n, name.data()) != ERROR_SUCCESS) return L"";
    return name.data();
}

static void OpenDefaultAppsSettings() {
    std::wstring uri = L"ms-settings:defaultapps";
    std::wstring pfn = PackageFamilyName();
    if (!pfn.empty()) uri += L"?registeredAUMID=" + pfn + L"%21PikViewer";
    ShellExecuteW(g.hwnd, L"open", uri.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

static float TopH() { return 48.0f * Dpi(); }
static float UiTop() { return g.showTop ? TopH() : 0.0f; }
static float AdjustPanelH() { return 390.0f * Dpi(); }
static D2D1_RECT_F AdjustPanelRect(float W) {
    float S = Dpi(), pw = 330.0f * S, ph = AdjustPanelH();
    float x = std::clamp(W - pw - 18.0f * S, 18.0f * S, std::max(18.0f * S, W - pw - 18.0f * S));
    return D2D1::RectF(x, TopH() + 6.0f*S, x + pw, TopH() + 6.0f*S + ph);
}
static bool PointInAdjustPanel(int x, int y) {
    RECT rc{}; GetClientRect(g.hwnd, &rc);
    auto r = AdjustPanelRect((float)rc.right);
    return g.showAdjust && In(r, x, y);
}
// Geometría de los deslizadores del panel de ajustes (dibujo, clics y arrastre).
struct AdjustGeo { float x0, x1, sx, resetL, valueL, ex, start, row; };
static AdjustGeo GetAdjustGeo(const D2D1_RECT_F& p, float S) {
    AdjustGeo a;
    a.x0 = p.left + 18 * S; a.x1 = p.right - 18 * S;
    a.sx = a.x0 + 92 * S;
    a.resetL = a.x1 - 34 * S;
    a.valueL = a.resetL - 58 * S;
    a.ex = a.valueL - 12 * S;
    a.start = p.top + 72 * S;
    a.row = 54 * S;
    return a;
}
static float& AdjustValue(int i) {
    return i == 0 ? g.gamma : i == 1 ? g.brightness : i == 2 ? g.contrast : g.saturation;
}
static float SliderValue(const AdjustGeo& A, int x) {
    float t = std::clamp((x - A.sx) / (A.ex - A.sx), 0.f, 1.f);
    return -1.f + t * 2.f;
}

static const UINT_PTR kToolbarTimer = 0x4D52;
static const UINT_PTR kCaretTimer = 0x4D53;
static const UINT_PTR kGifTimer = 0x4D54;
static void ArmToolbarTimer() { SetTimer(g.hwnd, kToolbarTimer, 180, nullptr); }
static void ArmCaretTimer() { SetTimer(g.hwnd, kCaretTimer, 530, nullptr); }
static void KillCaretTimer() { KillTimer(g.hwnd, kCaretTimer); }
static void UpdateToolbarHover(int y) {
    if (g.tool != T_NONE) {
        if (g.showTop) { g.showTop = false; g.showAdjust = false; g.hoverBtn = 0; Render(); }
        KillTimer(g.hwnd, kToolbarTimer);
        return;
    }
    if (y < TopH() || (g.showAdjust && y < TopH() + AdjustPanelH() + 12.0f * Dpi())) {
        if (!g.showTop) { g.showTop = true; Render(); }
        KillTimer(g.hwnd, kToolbarTimer);
    } else if (!g.mouseDown && g.adjustDrag < 0) {
        ArmToolbarTimer();
    }
}

// ------------------------------------------------------------------ utilidades
static std::wstring Fmt(const wchar_t* f, ...) {
    wchar_t b[256]; va_list a; va_start(a, f);
    _vsnwprintf_s(b, _countof(b), _TRUNCATE, f, a);
    va_end(a);
    return b;
}

static std::wstring LowerExt(const fs::path& p) {
    std::wstring e = p.extension().wstring();
    for (auto& c : e) c = towlower(c);
    return e;
}

static float HalfToFloat(uint16_t h) {
    uint32_t s = (uint32_t)(h & 0x8000u) << 16, e = (h >> 10) & 0x1f, m = h & 0x3ff, f;
    if (e == 0) {
        if (m == 0) f = s;
        else { e = 127 - 14; while (!(m & 0x400)) { m <<= 1; e--; } m &= 0x3ff; f = s | (e << 23) | (m << 13); }
    } else if (e == 31) f = s | 0x7f800000u | (m << 13);
    else f = s | ((e + 127 - 15) << 23) | (m << 13);
    float r; memcpy(&r, &f, 4); return r;
}

static float SrgbToLin(float c) { return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f); }
static uint16_t FloatToHalf(float x) {
    uint32_t bits; memcpy(&bits, &x, sizeof(bits));
    uint32_t sign = (bits >> 16) & 0x8000u;
    int exp = (int)((bits >> 23) & 0xffu) - 127 + 15;
    uint32_t mant = bits & 0x7fffffu;
    if (exp <= 0) {
        if (exp < -10) return (uint16_t)sign;
        mant |= 0x800000u;
        uint32_t shift = (uint32_t)(14 - exp);
        uint32_t halfMant = mant >> shift;
        if ((mant >> (shift - 1)) & 1u) halfMant++;
        return (uint16_t)(sign | halfMant);
    }
    if (exp >= 31) return (uint16_t)(sign | 0x7c00u);
    uint32_t halfMant = mant >> 13;
    if (mant & 0x1000u) {
        halfMant++;
        if (halfMant == 0x400u) { halfMant = 0; exp++; if (exp >= 31) return (uint16_t)(sign | 0x7c00u); }
    }
    return (uint16_t)(sign | ((uint32_t)exp << 10) | halfMant);
}

// Inversa de LinToEnc y misma curva que decode() del shader (sRGB por tramos, gamma 2.2 o 2.4).
// Hornear los ajustes con otra curva distinta a la del visor altera gamma, color y saturacion.
static float EncToLin(float c) {
    c = std::max(c, 0.f);
    if (g.eotf == 0) return SrgbToLin(c);
    return powf(c, g.eotf == 1 ? 2.2f : 2.4f);
}

// HDR -> SDR al exportar a PNG/JPG: en vez de recortar cada canal al blanco SDR (lo que quema las
// luces y desplaza los tonos), se comprime suavemente lo que supera la "rodilla" escalando el trio RGB
// por su canal maximo (conserva el tono). r,gg,b van relativos al blanco SDR (1.0 = blanco SDR);
// por debajo de la rodilla la imagen no cambia y el brillo maximo de la imagen (peak) queda en 1.0.
static void ToneMapToSdr(float& r, float& gg, float& b, float knee, float peak) {
    r = std::max(r, 0.f); gg = std::max(gg, 0.f); b = std::max(b, 0.f);
    float m = std::max(r, std::max(gg, b));
    if (m <= knee) return;
    float R = 1.f - knee;
    float L = std::max(peak - knee, R) / R;
    float x = (m - knee) / R;
    float mm = knee + R * x * (1.f + x / (L * L)) / (1.f + x);
    float k = mm / m;
    r *= k; gg *= k; b *= k;
}

static float LinToEnc(float v) {
    v = std::clamp(v, 0.f, 1.f);
    if (g.eotf == 0) return v <= 0.0031308f ? v * 12.92f : 1.055f * powf(v, 1.f / 2.4f) - 0.055f;
    return powf(v, g.eotf == 1 ? 1.f / 2.2f : 1.f / 2.4f);
}

// color de UI: sRGB -> lineal * blanco SDR (la UI se dibuja en la swap chain scRGB)
static D2D1_COLOR_F UiColor(uint32_t rgb, float a = 1.f) {
    float s = g.white;
    return D2D1::ColorF(SrgbToLin(((rgb >> 16) & 255) / 255.f) * s, SrgbToLin(((rgb >> 8) & 255) / 255.f) * s,
                        SrgbToLin((rgb & 255) / 255.f) * s, a);
}
// color de anotacion: HDR -> lineal*blanco SDR ; SDR -> valor codificado tal cual
static D2D1_COLOR_F AnnColor(uint32_t rgb, float a) {
    float r = ((rgb >> 16) & 255) / 255.f, gg = ((rgb >> 8) & 255) / 255.f, b = (rgb & 255) / 255.f;
    if (g.hdr) { float s = g.white; r = SrgbToLin(r) * s; gg = SrgbToLin(gg) * s; b = SrgbToLin(b) * s; }
    return D2D1::ColorF(r, gg, b, a);
}

// ------------------------------------------------------------------ nivel de blanco SDR
static void UpdateWhite() {
    g.white = 2.5f;
    UINT32 np = 0, nm = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) != ERROR_SUCCESS) return;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(np);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(nm);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, paths.data(), &nm, modes.data(), nullptr) != ERROR_SUCCESS) return;
    MONITORINFOEXW mi{}; mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromWindow(g.hwnd, MONITOR_DEFAULTTONEAREST), &mi);
    for (UINT32 i = 0; i < np; i++) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME sn{};
        sn.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        sn.header.size = sizeof(sn);
        sn.header.adapterId = paths[i].sourceInfo.adapterId;
        sn.header.id = paths[i].sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&sn.header) != ERROR_SUCCESS) continue;
        if (wcscmp(sn.viewGdiDeviceName, mi.szDevice) != 0) continue;
        DISPLAYCONFIG_SDR_WHITE_LEVEL wl{};
        wl.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
        wl.header.size = sizeof(wl);
        wl.header.adapterId = paths[i].targetInfo.adapterId;
        wl.header.id = paths[i].targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&wl.header) == ERROR_SUCCESS && wl.SDRWhiteLevel > 0)
            g.white = wl.SDRWhiteLevel / 1000.0f;
        return;
    }
}

// ------------------------------------------------------------------ vista / recorte
static CR CropRect() {
    for (auto it = g.ops.rbegin(); it != g.ops.rend(); ++it)
        if (it->type == T_CROP) {
            float x0 = std::min(it->pts[0].x, it->pts[1].x), y0 = std::min(it->pts[0].y, it->pts[1].y);
            float x1 = std::max(it->pts[0].x, it->pts[1].x), y1 = std::max(it->pts[0].y, it->pts[1].y);
            return { x0, y0, x1 - x0, y1 - y0 };
        }
    return { 0, 0, (float)g.imgW, (float)g.imgH };
}

static View GetView() {
    View v{};
    RECT rc; GetClientRect(g.hwnd, &rc);
    float W = (float)std::max<LONG>(1, rc.right), H = (float)std::max<LONG>(1, rc.bottom);
    v.c = CropRect();
    float ah = std::max(1.f, H);
    // La barra superior es una superposicion transparente: la imagen siempre ocupa
    // todo el area cliente y queda visible por debajo de los controles.
    float fit = std::min(W / std::max(1.f, v.c.w), ah / std::max(1.f, v.c.h));
    v.f = fit * std::clamp(g.zoom, 1.0f, 20.0f);
    v.dw = v.c.w * v.f; v.dh = v.c.h * v.f;
    // panX/panY son el desplazamiento del centro de la zona visible, en píxeles de imagen.
    float cx = v.c.x + v.c.w * 0.5f + g.panX;
    float cy = v.c.y + v.c.h * 0.5f + g.panY;
    v.dx = W * 0.5f - (cx - v.c.x) * v.f;
    v.dy = ah * 0.5f - (cy - v.c.y) * v.f;
    return v;
}
static P ToImg(const View& v, int x, int y) {
    float px = v.c.x + (x - v.dx) / v.f, py = v.c.y + (y - v.dy) / v.f;
    return { std::clamp(px, v.c.x, v.c.x + v.c.w), std::clamp(py, v.c.y, v.c.y + v.c.h) };
}
static D2D1_RECT_F Norm(P a, P b) { return { std::min(a.x, b.x), std::min(a.y, b.y), std::max(a.x, b.x), std::max(a.y, b.y) }; }

// ------------------------------------------------------------------ Direct3D / Direct2D
static void MakeTargets() {
    ComPtr<ID3D11Texture2D> bb;
    g.sc->GetBuffer(0, IID_PPV_ARGS(&bb));
    g.dev->CreateRenderTargetView(bb.Get(), nullptr, &g.rtv);
    ComPtr<IDXGISurface> s;
    g.sc->GetBuffer(0, IID_PPV_ARGS(&s));
    D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_R16G16B16A16_FLOAT, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.f, 96.f);
    g.d2d->CreateBitmapFromDxgiSurface(s.Get(), &bp, &g.bbBmp);
}

static void MakeFonts() {
    g.fCenter.Reset(); g.fLeft.Reset(); g.fWrap.Reset(); g.fIcon.Reset();
    float s = Dpi();
    auto make = [&](const wchar_t* family, float size, ComPtr<IDWriteTextFormat>& out) {
        g.dw->CreateTextFormat(family, nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                               DWRITE_FONT_STRETCH_NORMAL, size * s, UiLocaleName(), &out);
    };
    make(L"Segoe UI", 13.f, g.fCenter);
    make(L"Segoe UI", 13.f, g.fLeft);
    make(L"Segoe UI", 13.f, g.fWrap);
    make(L"Segoe MDL2 Assets", 17.f, g.fIcon);
    for (auto f : { g.fCenter.Get(), g.fLeft.Get(), g.fWrap.Get(), g.fIcon.Get() }) {
        if (!f) continue;
        f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }
    if (g.fCenter) g.fCenter->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    if (g.fIcon) g.fIcon->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    if (g.fLeft) g.fLeft->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    if (g.fWrap) {
        g.fWrap->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        g.fWrap->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    }
}

static bool InitD3D() {
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT; // necesario para Direct2D
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0,
                                 D3D11_SDK_VERSION, &g.dev, nullptr, &g.ctx))) return false;
    ComPtr<IDXGIDevice> dxd; g.dev.As(&dxd);
    ComPtr<IDXGIAdapter> ad; dxd->GetAdapter(&ad);
    ComPtr<IDXGIFactory2> fac; ad->GetParent(IID_PPV_ARGS(&fac));

    RECT rc; GetClientRect(g.hwnd, &rc);
    DXGI_SWAP_CHAIN_DESC1 d{};
    d.Width = std::max<LONG>(1, rc.right); d.Height = std::max<LONG>(1, rc.bottom);
    d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    d.SampleDesc.Count = 1;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d.BufferCount = 2;
    d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain1> sc1;
    if (FAILED(fac->CreateSwapChainForHwnd(g.dev.Get(), g.hwnd, &d, nullptr, nullptr, &sc1))) return false;
    sc1.As(&g.sc);
    g.sc->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709); // scRGB
    fac->MakeWindowAssociation(g.hwnd, DXGI_MWA_NO_ALT_ENTER);

    // Direct2D + DirectWrite
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), nullptr,
                                 (void**)g.d2dF.GetAddressOf()))) return false;
    if (FAILED(g.d2dF->CreateDevice(dxd.Get(), &g.d2dDev))) return false;
    if (FAILED(g.d2dDev->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &g.d2d))) return false;
    g.d2d->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &g.brush);
    g.d2dF->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
        D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND), nullptr, 0, &g.ssRound);
    g.d2dF->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_SQUARE, D2D1_CAP_STYLE_SQUARE,
        D2D1_CAP_STYLE_SQUARE, D2D1_LINE_JOIN_ROUND), nullptr, 0, &g.ssSquare);
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)g.dw.GetAddressOf());
    MakeFonts();
    MakeTargets();

    ComPtr<ID3DBlob> vb, pb, err;
    if (FAILED(D3DCompile(kShader, strlen(kShader), nullptr, nullptr, nullptr, "VS", "vs_5_0", 0, 0, &vb, &err)) ||
        FAILED(D3DCompile(kShader, strlen(kShader), nullptr, nullptr, nullptr, "PS", "ps_5_0", 0, 0, &pb, &err))) {
        MessageBoxA(g.hwnd, err ? (const char*)err->GetBufferPointer() : "Error de shader", "PikViewer", MB_ICONERROR);
        return false;
    }
    g.dev->CreateVertexShader(vb->GetBufferPointer(), vb->GetBufferSize(), nullptr, &g.vs);
    g.dev->CreatePixelShader(pb->GetBufferPointer(), pb->GetBufferSize(), nullptr, &g.ps);

    D3D11_BUFFER_DESC bd{}; bd.ByteWidth = sizeof(Cbuf); bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    g.dev->CreateBuffer(&bd, nullptr, &g.cb);

    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    g.dev->CreateSamplerState(&sd, &g.smp);

    D3D11_RASTERIZER_DESC rd{}; rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE;
    g.dev->CreateRasterizerState(&rd, &g.rast);
    return true;
}

// Direct2D puede alterar el estado de D3D11: se reestablece en cada frame
static void SetupPipeline() {
    g.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    g.ctx->IASetInputLayout(nullptr);
    g.ctx->VSSetShader(g.vs.Get(), nullptr, 0);
    g.ctx->VSSetConstantBuffers(0, 1, g.cb.GetAddressOf());
    g.ctx->PSSetShader(g.ps.Get(), nullptr, 0);
    g.ctx->PSSetConstantBuffers(0, 1, g.cb.GetAddressOf());
    g.ctx->PSSetSamplers(0, 1, g.smp.GetAddressOf());
    g.ctx->RSSetState(g.rast.Get());
    g.ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
}

static void Resize() {
    if (!g.sc) return;
    RECT rc; GetClientRect(g.hwnd, &rc);
    g.ctx->OMSetRenderTargets(0, nullptr, nullptr);
    g.d2d->SetTarget(nullptr);
    g.bbBmp.Reset();
    g.rtv.Reset();
    g.sc->ResizeBuffers(0, std::max<LONG>(1, rc.right), std::max<LONG>(1, rc.bottom), DXGI_FORMAT_UNKNOWN, 0);
    g.sc->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709);
    MakeTargets();
}

// ------------------------------------------------------------------ anotaciones: dibujo sobre la textura
static void DrawOp(const Op& o) {
    if (o.type == T_CROP || o.pts.empty()) return;
    float a = (o.type == T_HIGH) ? 0.4f : 1.f;
    g.brush->SetColor(AnnColor(o.color, a));
    ID2D1StrokeStyle* ss = (o.type == T_HIGH) ? g.ssSquare.Get() : g.ssRound.Get();
    switch (o.type) {
    case T_PEN: case T_HIGH: {
        ComPtr<ID2D1PathGeometry> pg; g.d2dF->CreatePathGeometry(&pg);
        ComPtr<ID2D1GeometrySink> sink; pg->Open(&sink);
        sink->BeginFigure(o.pts[0], D2D1_FIGURE_BEGIN_HOLLOW);
        if (o.pts.size() == 1) sink->AddLine(P{ o.pts[0].x + 0.01f, o.pts[0].y });
        for (size_t i = 1; i < o.pts.size(); i++) sink->AddLine(o.pts[i]);
        sink->EndFigure(D2D1_FIGURE_END_OPEN);
        sink->Close();
        g.d2d->DrawGeometry(pg.Get(), g.brush.Get(), o.width, ss); // un solo trazo: sin doble mezcla
        break;
    }
    case T_RECT: g.d2d->DrawRectangle(Norm(o.pts[0], o.pts[1]), g.brush.Get(), o.width, ss); break;
    case T_ELL: {
        D2D1_RECT_F r = Norm(o.pts[0], o.pts[1]);
        g.d2d->DrawEllipse(D2D1::Ellipse(P{ (r.left + r.right) / 2, (r.top + r.bottom) / 2 },
                                         (r.right - r.left) / 2, (r.bottom - r.top) / 2), g.brush.Get(), o.width, ss);
        break;
    }
    case T_LINE: g.d2d->DrawLine(o.pts[0], o.pts[1], g.brush.Get(), o.width, ss); break;
    case T_ARROW: {
        P p0 = o.pts[0], p1 = o.pts[1];
        g.d2d->DrawLine(p0, p1, g.brush.Get(), o.width, ss);
        float dx = p1.x - p0.x, dy = p1.y - p0.y, L = sqrtf(dx * dx + dy * dy);
        if (L > 1e-3f) {
            dx /= L; dy /= L;
            float hl = o.width * 4.5f + 4.f;
            for (float ang : { 0.5f, -0.5f }) {
                float c = cosf(ang), s = sinf(ang);
                P h{ p1.x - (dx * c - dy * s) * hl, p1.y - (dx * s + dy * c) * hl };
                g.d2d->DrawLine(p1, h, g.brush.Get(), o.width, ss);
            }
        }
        break;
    }
    }
}

// base + anotaciones -> work -> disp (con mips)
static void Redraw(const Op* extra) {
    if (!g.work || !g.disp) return;
    g.ctx->CopySubresourceRegion(g.work.Get(), 0, 0, 0, 0, g.base.Get(), 0, nullptr);
    if (g.workBmp && (!g.ops.empty() || extra)) {
        g.d2d->SetTarget(g.workBmp.Get());
        g.d2d->BeginDraw();
        g.d2d->SetTransform(D2D1::Matrix3x2F::Identity());
        for (auto& o : g.ops) DrawOp(o);
        if (extra) DrawOp(*extra);
        g.d2d->EndDraw();
    }
    g.ctx->CopySubresourceRegion(g.disp.Get(), 0, 0, 0, 0, g.work.Get(), 0, nullptr);
    g.ctx->GenerateMips(g.srv.Get());
}

// ------------------------------------------------------------------ carga de imagen
static bool IsOneOf(const GUID& f, std::initializer_list<const GUID*> l) {
    for (auto p : l) if (IsEqualGUID(f, *p)) return true;
    return false;
}

static std::wstring FriendlyName(IWICComponentInfo* ci) {
    if (!ci) return L"";
    UINT n = 0; ci->GetFriendlyName(0, nullptr, &n);
    std::wstring s(n, L'\0'); ci->GetFriendlyName(n, s.data(), &n);
    while (!s.empty() && s.back() == L'\0') s.pop_back();
    return s;
}

static void UpdateTitle() {
    if (g.idx >= 0 && g.idx < (int)g.files.size()) {
        std::wstring t = g.files[g.idx].filename().wstring() + ((g.ops.empty() && !g.adjustDirty && !g.resizeDirty) ? L"" : L" *");
        SetWindowTextW(g.hwnd, t.c_str());
    } else {
        SetWindowTextW(g.hwnd, L"PikViewer");
    }
}

static void SaveEotf() {
    DWORD v = g.eotf;
    RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\PikViewer", L"Eotf", REG_DWORD, &v, sizeof(v));
}

// ------------------------------------------------------------------ GIF animado
static bool GifProp(IWICMetadataQueryReader* q, const wchar_t* name, UINT& out) {
    if (!q) return false;
    PROPVARIANT v; PropVariantInit(&v);
    if (FAILED(q->GetMetadataByName(name, &v))) return false;
    bool ok = true;
    switch (v.vt) {
    case VT_UI1: out = v.bVal; break;
    case VT_UI2: out = v.uiVal; break;
    case VT_UI4: out = v.ulVal; break;
    case VT_BOOL: out = v.boolVal ? 1u : 0u; break;
    default: ok = false;
    }
    PropVariantClear(&v);
    return ok;
}

// Decodifica todos los fotogramas de un GIF compuestos sobre el lienzo (respeta posicion,
// transparencia y metodo de eliminacion). Devuelve false si es estatico o demasiado grande.
static bool LoadGifFrames(IWICBitmapDecoder* dec, std::vector<std::vector<BYTE>>& frames,
                          std::vector<UINT>& delays, UINT& W, UINT& H) {
    UINT n = 0;
    if (FAILED(dec->GetFrameCount(&n)) || n < 2) return false;
    ComPtr<IWICMetadataQueryReader> dq; dec->GetMetadataQueryReader(&dq);
    UINT cw = 0, ch = 0;
    GifProp(dq.Get(), L"/logscrdesc/Width", cw);
    GifProp(dq.Get(), L"/logscrdesc/Height", ch);
    if (!cw || !ch) { ComPtr<IWICBitmapFrameDecode> f0; if (FAILED(dec->GetFrame(0, &f0)) || FAILED(f0->GetSize(&cw, &ch))) return false; }
    if (!cw || !ch || cw > 16384 || ch > 16384) return false;
    if ((unsigned long long)cw * ch * 4ull * n > 768ull * 1024 * 1024) return false; // demasiada memoria: se muestra estatico

    std::vector<BYTE> canvas((size_t)cw * ch * 4, 0), saved;
    int prevDisp = 0; RECT prevR{ 0, 0, 0, 0 };
    std::vector<std::vector<BYTE>> outF; std::vector<UINT> outD;
    outF.reserve(n);
    for (UINT i = 0; i < n; i++) {
        ComPtr<IWICBitmapFrameDecode> fr;
        if (FAILED(dec->GetFrame(i, &fr))) return false;
        ComPtr<IWICMetadataQueryReader> q; fr->GetMetadataQueryReader(&q);
        UINT fw = 0, fh = 0, left = 0, top = 0, disp = 0, delay = 0;
        fr->GetSize(&fw, &fh);
        GifProp(q.Get(), L"/imgdesc/Left", left); GifProp(q.Get(), L"/imgdesc/Top", top);
        GifProp(q.Get(), L"/imgdesc/Width", fw);  GifProp(q.Get(), L"/imgdesc/Height", fh);
        GifProp(q.Get(), L"/grctlext/Disposal", disp);
        GifProp(q.Get(), L"/grctlext/Delay", delay);
        if (!fw || !fh) return false;

        // eliminacion del fotograma anterior
        if (i > 0) {
            if (prevDisp == 2) {
                for (LONG y = prevR.top; y < prevR.bottom && y < (LONG)ch; y++)
                    for (LONG x = prevR.left; x < prevR.right && x < (LONG)cw; x++)
                        memset(&canvas[((size_t)y * cw + x) * 4], 0, 4);
            } else if (prevDisp == 3 && saved.size() == canvas.size()) canvas = saved;
        }
        if (disp == 3) saved = canvas;

        ComPtr<IWICFormatConverter> cv;
        if (FAILED(g.wic->CreateFormatConverter(&cv)) ||
            FAILED(cv->Initialize(fr.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) return false;
        std::vector<BYTE> fb((size_t)fw * fh * 4);
        if (FAILED(cv->CopyPixels(nullptr, fw * 4, (UINT)fb.size(), fb.data()))) return false;
        for (UINT y = 0; y < fh && top + y < ch; y++)
            for (UINT x = 0; x < fw && left + x < cw; x++) {
                const BYTE* sp = &fb[((size_t)y * fw + x) * 4];
                if (sp[3] == 0) continue; // pixel transparente: se ve lo que ya había
                memcpy(&canvas[((size_t)(top + y) * cw + (left + x)) * 4], sp, 4);
            }
        outF.push_back(canvas);
        UINT ms = delay * 10; if (ms < 20) ms = 100; // convencion de los navegadores
        outD.push_back(ms);
        prevDisp = (int)disp; prevR = RECT{ (LONG)left, (LONG)top, (LONG)(left + fw), (LONG)(top + fh) };
    }
    frames = std::move(outF); delays = std::move(outD); W = cw; H = ch;
    return true;
}

static void GifStop() {
    if (g.hwnd) KillTimer(g.hwnd, kGifTimer);
    g.gifFrames.clear(); g.gifFrames.shrink_to_fit(); g.gifDelay.clear(); g.gifIdx = 0; g.gifBase = nullptr;
}

// La animacion se pausa mientras haya edicion en curso (anotaciones, recorte, ajustes, redimensionado).
static bool GifEditing() {
    return !g.ops.empty() || g.adjustDirty || g.resizeDirty || g.drawing || g.cropping || g.tool != T_NONE || g.dialog != 0;
}

static void GifTick() {
    if (g.gifFrames.empty() || g.base.Get() != g.gifBase ||
        g.gifFrames[0].size() != (size_t)g.imgW * g.imgH * 4) { GifStop(); return; }
    if (!GifEditing() && !IsIconic(g.hwnd)) {
        g.gifIdx = (g.gifIdx + 1) % g.gifFrames.size();
        g.ctx->UpdateSubresource(g.base.Get(), 0, nullptr, g.gifFrames[g.gifIdx].data(), g.imgW * 4, 0);
        Redraw(nullptr);
        Render();
    }
    SetTimer(g.hwnd, kGifTimer, g.gifDelay[g.gifIdx], nullptr);
}

static bool LoadPicture(const fs::path& p) {
    ComPtr<IWICBitmapDecoder> dec;
    if (FAILED(g.wic->CreateDecoderFromFilename(p.c_str(), nullptr, GENERIC_READ,
                                                WICDecodeMetadataCacheOnDemand, &dec))) return false;
    ComPtr<IWICBitmapFrameDecode> fr;
    if (FAILED(dec->GetFrame(0, &fr))) return false;
    WICPixelFormatGUID pf; fr->GetPixelFormat(&pf);

    bool hdr = IsOneOf(pf, { &GUID_WICPixelFormat64bppRGBAHalf, &GUID_WICPixelFormat64bppRGBHalf,
        &GUID_WICPixelFormat48bppRGBHalf, &GUID_WICPixelFormat128bppRGBAFloat, &GUID_WICPixelFormat128bppRGBFloat,
        &GUID_WICPixelFormat96bppRGBFloat, &GUID_WICPixelFormat64bppRGBAFixedPoint,
        &GUID_WICPixelFormat64bppRGBFixedPoint, &GUID_WICPixelFormat48bppRGBFixedPoint,
        &GUID_WICPixelFormat128bppRGBAFixedPoint, &GUID_WICPixelFormat128bppRGBFixedPoint,
        &GUID_WICPixelFormat96bppRGBFixedPoint, &GUID_WICPixelFormat32bppRGBE });
    bool deep = !hdr && IsOneOf(pf, { &GUID_WICPixelFormat64bppRGBA, &GUID_WICPixelFormat64bppRGB,
        &GUID_WICPixelFormat48bppRGB, &GUID_WICPixelFormat64bppPRGBA, &GUID_WICPixelFormat16bppGray });

    REFWICPixelFormatGUID target = hdr ? GUID_WICPixelFormat64bppRGBAHalf
                                 : deep ? GUID_WICPixelFormat64bppRGBA : GUID_WICPixelFormat32bppRGBA;
    DXGI_FORMAT fmt = hdr ? DXGI_FORMAT_R16G16B16A16_FLOAT
                    : deep ? DXGI_FORMAT_R16G16B16A16_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
    UINT bpp = (hdr || deep) ? 8 : 4;

    UINT w = 0, h = 0, stride = 0;
    std::vector<BYTE> buf;
    std::vector<std::vector<BYTE>> gFrames; std::vector<UINT> gDelay;
    if (!hdr && !deep && LowerExt(p) == L".gif" && LoadGifFrames(dec.Get(), gFrames, gDelay, w, h)) {
        stride = w * 4; buf = gFrames[0]; // GIF animado: el primer fotograma ya compuesto sobre el lienzo
    } else {
        gFrames.clear(); gDelay.clear();
        ComPtr<IWICFormatConverter> cv;
        g.wic->CreateFormatConverter(&cv);
        if (FAILED(cv->Initialize(fr.Get(), target, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) return false;
        cv->GetSize(&w, &h);
        if (!w || !h || w > 16384 || h > 16384) return false;
        stride = w * bpp;
        buf.resize((size_t)stride * h);
        if (FAILED(cv->CopyPixels(nullptr, stride, (UINT)buf.size(), buf.data()))) return false;
    }

    // --- texturas
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1; td.Format = fmt;
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd{ buf.data(), stride, 0 };
    ComPtr<ID3D11Texture2D> base, work, disp;
    if (FAILED(g.dev->CreateTexture2D(&td, &sd, &base))) return false;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    if (FAILED(g.dev->CreateTexture2D(&td, nullptr, &work))) return false;
    td.MipLevels = 0; td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
    if (FAILED(g.dev->CreateTexture2D(&td, nullptr, &disp))) return false;
    ComPtr<ID3D11ShaderResourceView> sv;
    if (FAILED(g.dev->CreateShaderResourceView(disp.Get(), nullptr, &sv))) return false;

    // --- bitmap D2D sobre "work" (para dibujar anotaciones)
    ComPtr<ID2D1Bitmap1> wb;
    {
        ComPtr<IDXGISurface> ws; work.As(&ws);
        D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(fmt, hdr ? D2D1_ALPHA_MODE_PREMULTIPLIED : D2D1_ALPHA_MODE_IGNORE), 96.f, 96.f);
        if (FAILED(g.d2d->CreateBitmapFromDxgiSurface(ws.Get(), &bp, &wb))) wb.Reset();
    }

    // --- informacion
    Info in;
    {
        ComPtr<IWICBitmapDecoderInfo> di; ComPtr<IWICComponentInfo> ci;
        if (SUCCEEDED(dec->GetDecoderInfo(&di)) && SUCCEEDED(di.As(&ci))) in.container = FriendlyName(ci.Get());
        ComPtr<IWICComponentInfo> pi;
        if (SUCCEEDED(g.wic->CreateComponentInfo(pf, &pi))) in.pixfmt = FriendlyName(pi.Get());
        std::error_code ec; in.fsize = fs::file_size(p, ec);
        if (hdr) in.cs = L"scRGB (Rec.709 lineal, gama amplia)";
        else {
            in.cs = L"Sin perfil (se asume sRGB)";
            UINT n = 0; fr->GetColorContexts(0, nullptr, &n);
            if (n > 0) {
                ComPtr<IWICColorContext> cc; g.wic->CreateColorContext(&cc);
                IWICColorContext* arr[1] = { cc.Get() }; UINT n2 = 0;
                if (SUCCEEDED(fr->GetColorContexts(1, arr, &n2))) {
                    WICColorContextType t; cc->GetType(&t);
                    if (t == WICColorContextProfile) in.cs = L"Perfil ICC incrustado (no aplicado)";
                    else if (t == WICColorContextExifColorSpace) {
                        UINT cs = 0; cc->GetExifColorSpace(&cs);
                        in.cs = cs == 1 ? L"sRGB" : L"Adobe RGB (no aplicado)";
                    }
                }
            }
        }
        if (hdr) {
            const uint16_t* hp = (const uint16_t*)buf.data();
            double sum = 0; float mx = 0; size_t np = (size_t)w * h;
            for (size_t i = 0; i < np; i++) {
                float r = HalfToFloat(hp[4 * i]), gg = HalfToFloat(hp[4 * i + 1]), b = HalfToFloat(hp[4 * i + 2]);
                float y = 0.2126f * r + 0.7152f * gg + 0.0722f * b;
                if (y > mx) mx = y;
                if (y > 0) sum += y;
            }
            in.maxN = mx * 80.f; in.avgN = (float)(sum / np) * 80.f;
        }
    }

    g.base = base; g.work = work; g.disp = disp; g.srv = sv; g.workBmp = wb;
    g.imgW = w; g.imgH = h; g.hdr = hdr; g.deep = deep; g.canEdit = (wb != nullptr);
    g.info = in;
    g.ops.clear(); g.undoS.clear(); g.redoS.clear();
    GifStop();
    if (!gFrames.empty()) {
        g.gifFrames = std::move(gFrames); g.gifDelay = std::move(gDelay); g.gifIdx = 0; g.gifBase = g.base.Get();
        SetTimer(g.hwnd, kGifTimer, g.gifDelay[0], nullptr);
    }
    // Tras cargar (o recargar al guardar) los píxeles ya son la imagen final:
    // hay que poner los ajustes a neutro para no volver a aplicarlos en el shader.
    g.gamma = 0.f; g.brightness = 0.f; g.contrast = 0.f; g.saturation = 0.f;
    g.adjustDirty = false;
    g.resizeDirty = false; g.resizeW = (int)w; g.resizeH = (int)h; g.resizeWText = std::to_wstring(w); g.resizeHText = std::to_wstring(h); g.resizeField = 0; g.resizeSelectAll = false; KillCaretTimer();
    g.cropping = false; g.drawing = false;
    g.zoom = 1.0f; g.panX = 0.0f; g.panY = 0.0f; g.panning = false;
    Redraw(nullptr);
    UpdateTitle();
    return true;
}

static bool IsImage(const fs::path& p) {
    std::wstring e = LowerExt(p);
    for (auto x : kExts) if (e == x) return true;
    return false;
}

// ------------------------------------------------------------------ guardar
static bool WriteImage(const fs::path& path) {
    std::wstring ext = LowerExt(path);
    const GUID* cont = nullptr;
    if (ext == L".png") cont = &GUID_ContainerFormatPng;
    else if (ext == L".jpg" || ext == L".jpeg" || ext == L".jfif") cont = &GUID_ContainerFormatJpeg;
    else if (ext == L".bmp") cont = &GUID_ContainerFormatBmp;
    else if (ext == L".tif" || ext == L".tiff") cont = &GUID_ContainerFormatTiff;
    else if (ext == L".jxr") cont = &GUID_ContainerFormatWmp;
    if (!cont) { MessageBoxW(g.hwnd, T(L"Formato de salida no soportado."), L"PikViewer", MB_ICONWARNING); return false; }

    CR c = CropRect();
    UINT cx = (UINT)c.x, cy = (UINT)c.y, cw = std::max(1u, (UINT)c.w), ch = std::max(1u, (UINT)c.h);
    if (cx + cw > g.imgW) cw = g.imgW - cx;
    if (cy + ch > g.imgH) ch = g.imgH - cy;

    D3D11_TEXTURE2D_DESC d{}; g.work->GetDesc(&d);
    d.Width = cw; d.Height = ch; d.MipLevels = 1; d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE; d.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> st;
    if (FAILED(g.dev->CreateTexture2D(&d, nullptr, &st))) return false;
    D3D11_BOX box{ cx, cy, 0, cx + cw, cy + ch, 1 };
    g.ctx->CopySubresourceRegion(st.Get(), 0, 0, 0, 0, g.work.Get(), 0, &box);
    D3D11_MAPPED_SUBRESOURCE ms;
    if (FAILED(g.ctx->Map(st.Get(), 0, D3D11_MAP_READ_WRITE, 0, &ms))) return false;

    // Los ajustes (gamma/brillo/contraste/saturación) son efectos de visualización
    // en el shader, por lo que g.work por sí solo todavía contiene la imagen original.
    // Al guardar debemos "hornear" esos ajustes en los píxeles que van al archivo.
    if (g.adjustDirty) {
        const float levelsGamma = exp2f(g.gamma);
        const float gammaInv = 1.0f / std::max(levelsGamma, 0.001f);
        // Brillo perceptual: mantiene negro/blanco y actúa principalmente sobre medios tonos.
        const float brightness = g.brightness;
        const float appliedContrast = 1.0f + g.contrast * 0.1f;
        const float white = std::max(g.white, 1.0f);
        for (UINT y = 0; y < ch; ++y) {
            BYTE* row = (BYTE*)ms.pData + (size_t)y * ms.RowPitch;
            for (UINT x = 0; x < cw; ++x) {
                float r, gg, b, a;
                if (g.hdr) {
                    const uint16_t* q = (const uint16_t*)row + x * 4;
                    r = HalfToFloat(q[0]); gg = HalfToFloat(q[1]); b = HalfToFloat(q[2]); a = HalfToFloat(q[3]);
                } else if (g.deep) {
                    const uint16_t* q = (const uint16_t*)row + x * 4;
                    r = q[0] / 65535.0f; gg = q[1] / 65535.0f; b = q[2] / 65535.0f; a = q[3] / 65535.0f;
                } else {
                    const BYTE* q = row + x * 4;
                    r = q[0] / 255.0f; gg = q[1] / 255.0f; b = q[2] / 255.0f; a = q[3] / 255.0f;
                }

                float nr, ng, nb;
                if (!g.hdr) {
                    r = powf(std::max(r, 0.0f), gammaInv);
                    gg = powf(std::max(gg, 0.0f), gammaInv);
                    b = powf(std::max(b, 0.0f), gammaInv);
                    nr = EncToLin(r); ng = EncToLin(gg); nb = EncToLin(b);
                } else {
                    nr = r / white; ng = gg / white; nb = b / white;
                    nr = powf(std::max(nr, 0.0f), gammaInv);
                    ng = powf(std::max(ng, 0.0f), gammaInv);
                    nb = powf(std::max(nb, 0.0f), gammaInv);
                }
                nr = nr + brightness * nr * (1.0f - nr);
                ng = ng + brightness * ng * (1.0f - ng);
                nb = nb + brightness * nb * (1.0f - nb);
                nr = (nr - 0.5f) * appliedContrast + 0.5f;
                ng = (ng - 0.5f) * appliedContrast + 0.5f;
                nb = (nb - 0.5f) * appliedContrast + 0.5f;
                float lum = 0.2126f * nr + 0.7152f * ng + 0.0722f * nb;
                float sat = 1.0f + g.saturation;
                nr = lum + (nr - lum) * sat;
                ng = lum + (ng - lum) * sat;
                nb = lum + (nb - lum) * sat;
                nr = std::max(nr, 0.0f); ng = std::max(ng, 0.0f); nb = std::max(nb, 0.0f);

                if (g.hdr) {
                    uint16_t* q = (uint16_t*)row + x * 4;
                    q[0] = FloatToHalf(nr * white); q[1] = FloatToHalf(ng * white); q[2] = FloatToHalf(nb * white); q[3] = FloatToHalf(std::clamp(a, 0.0f, 1.0f));
                } else if (g.deep) {
                    uint16_t* q = (uint16_t*)row + x * 4;
                    q[0] = (uint16_t)(LinToEnc(nr) * 65535.0f + 0.5f);
                    q[1] = (uint16_t)(LinToEnc(ng) * 65535.0f + 0.5f);
                    q[2] = (uint16_t)(LinToEnc(nb) * 65535.0f + 0.5f);
                    q[3] = (uint16_t)(std::clamp(a, 0.0f, 1.0f) * 65535.0f + 0.5f);
                } else {
                    BYTE* q = row + x * 4;
                    q[0] = (BYTE)(LinToEnc(nr) * 255.0f + 0.5f);
                    q[1] = (BYTE)(LinToEnc(ng) * 255.0f + 0.5f);
                    q[2] = (BYTE)(LinToEnc(nb) * 255.0f + 0.5f);
                    q[3] = (BYTE)(std::clamp(a, 0.0f, 1.0f) * 255.0f + 0.5f);
                }
            }
        }
    }

    UINT bpp = (g.hdr || g.deep) ? 8 : 4;
    WICPixelFormatGUID sf = g.hdr ? GUID_WICPixelFormat64bppRGBAHalf
                          : g.deep ? GUID_WICPixelFormat64bppRGBA : GUID_WICPixelFormat32bppRGBA;
    ComPtr<IWICBitmap> bmp;
    std::vector<BYTE> conv;
    HRESULT hr;
    if (g.hdr && ext != L".jxr") { // HDR -> SDR: se recorta al blanco SDR
        conv.resize((size_t)cw * ch * 4);
        const float knee = 0.8f;
        float peak = 1.0f; // brillo maximo de la imagen (relativo al blanco SDR), acotado para que un pixel suelto no domine
        for (UINT y = 0; y < ch; y++) {
            const uint16_t* row = (const uint16_t*)((const BYTE*)ms.pData + (size_t)y * ms.RowPitch);
            for (UINT x = 0; x < cw; x++)
                for (int k = 0; k < 3; k++) peak = std::max(peak, HalfToFloat(row[4 * x + k]) / g.white);
        }
        peak = std::min(peak, 16.0f);
        for (UINT y = 0; y < ch; y++) {
            const uint16_t* row = (const uint16_t*)((const BYTE*)ms.pData + (size_t)y * ms.RowPitch);
            for (UINT x = 0; x < cw; x++) {
                BYTE* o = &conv[((size_t)y * cw + x) * 4];
                float c3[3] = { HalfToFloat(row[4 * x]) / g.white, HalfToFloat(row[4 * x + 1]) / g.white, HalfToFloat(row[4 * x + 2]) / g.white };
                ToneMapToSdr(c3[0], c3[1], c3[2], knee, peak);
                for (int k = 0; k < 3; k++) o[k] = (BYTE)(LinToEnc(c3[k]) * 255.f + 0.5f);
                o[3] = (BYTE)(std::clamp(HalfToFloat(row[4 * x + 3]), 0.f, 1.f) * 255.f + 0.5f);
            }
        }
        sf = GUID_WICPixelFormat32bppRGBA;
        hr = g.wic->CreateBitmapFromMemory(cw, ch, sf, cw * 4, (UINT)conv.size(), conv.data(), &bmp);
    } else {
        hr = g.wic->CreateBitmapFromMemory(cw, ch, sf, ms.RowPitch, ms.RowPitch * (ch - 1) + cw * bpp,
                                           (BYTE*)ms.pData, &bmp);
    }
    g.ctx->Unmap(st.Get(), 0);
    if (FAILED(hr)) return false;

    // Redimensionado final: solo si el tamaño pedido difiere del bitmap actual
    // (si ya se aplicó en vivo, imgW/H coinciden y no se vuelve a escalar).
    if (g.resizeDirty && g.resizeW > 0 && g.resizeH > 0 &&
        ((UINT)g.resizeW != cw || (UINT)g.resizeH != ch)) {
        ComPtr<IWICBitmapScaler> scaler;
        if (FAILED(g.wic->CreateBitmapScaler(&scaler)) ||
            FAILED(scaler->Initialize(bmp.Get(), (UINT)g.resizeW, (UINT)g.resizeH, WICBitmapInterpolationModeFant))) {
            MessageBoxW(g.hwnd, T(L"No se pudo redimensionar la imagen."), L"PikViewer", MB_ICONWARNING);
            return false;
        }
        ComPtr<IWICBitmap> scaled;
        if (FAILED(g.wic->CreateBitmapFromSource(scaler.Get(), WICBitmapCacheOnLoad, &scaled))) {
            MessageBoxW(g.hwnd, T(L"No se pudo redimensionar la imagen."), L"PikViewer", MB_ICONWARNING);
            return false;
        }
        bmp = scaled;
    }

    // Usar siempre el tamaño real del bitmap final (evita desajustes tras resize en vivo).
    UINT outW = 0, outH = 0;
    if (FAILED(bmp->GetSize(&outW, &outH)) || !outW || !outH) return false;

    ComPtr<IWICStream> stream; g.wic->CreateStream(&stream);
    if (FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE))) {
        MessageBoxW(g.hwnd, T(L"No se pudo escribir el archivo."), L"PikViewer", MB_ICONWARNING); return false;
    }
    ComPtr<IWICBitmapEncoder> enc;
    if (FAILED(g.wic->CreateEncoder(*cont, nullptr, &enc))) return false;
    if (FAILED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache))) return false;
    ComPtr<IWICBitmapFrameEncode> fe; ComPtr<IPropertyBag2> pb;
    if (FAILED(enc->CreateNewFrame(&fe, &pb))) return false;
    if (*cont == GUID_ContainerFormatJpeg || *cont == GUID_ContainerFormatWmp) {
        PROPBAG2 opt{}; opt.pstrName = (LPOLESTR)L"ImageQuality";
        VARIANT v; VariantInit(&v); v.vt = VT_R4; v.fltVal = (*cont == GUID_ContainerFormatWmp) ? 1.0f : 0.95f;
        pb->Write(1, &opt, &v);
    }
    if (FAILED(fe->Initialize(pb.Get()))) return false;
    if (FAILED(fe->SetSize(outW, outH))) return false;
    WICPixelFormatGUID pf = sf;
    if (FAILED(fe->SetPixelFormat(&pf))) return false;
    ComPtr<IWICBitmapSource> src = bmp;
    if (!IsEqualGUID(pf, sf)) {
        ComPtr<IWICFormatConverter> cv; g.wic->CreateFormatConverter(&cv);
        if (FAILED(cv->Initialize(bmp.Get(), pf, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) return false;
        src = cv;
    }
    if (FAILED(fe->WriteSource(src.Get(), nullptr))) return false;
    if (FAILED(fe->Commit())) return false;
    if (FAILED(enc->Commit())) return false;
    // Liberar el archivo antes de que el llamador intente renombrarlo/reemplazarlo.
    fe.Reset(); pb.Reset(); enc.Reset(); stream.Reset();
    return true;
}

static void OpenPath(fs::path p);
static int RunConfirmDialog(int kind);
static App::Hist CaptureHist(bool withImage);

// Recarga el archivo recien guardado conservando el historial: el estado previo al guardado
// (imagen original + anotaciones + ajustes) pasa a ser un paso de "deshacer", de modo que
// deshacer/rehacer siguen funcionando despues de guardar.
static void ReopenKeepHistory(const fs::path& p) {
    App::Hist pre = CaptureHist(true);
    std::vector<App::Hist> older = std::move(g.undoS);
    g.undoS.clear();
    ID3D11Texture2D* before = g.base.Get();
    OpenPath(p); // LoadPicture limpia ops e historial
    if (g.base.Get() == before) { g.undoS = std::move(older); return; } // no se pudo recargar: nada cambia
    if (!pre.hasImg) return; // sin snapshot de la imagen no se puede restaurar el estado previo
    g.undoS = std::move(older);
    g.undoS.push_back(std::move(pre));
    Render();
}

static bool DoSaveAs() {
    if (!g.srv || g.files.empty()) return false;
    fs::path cur = g.files[g.idx];
    std::wstring stem = cur.stem().wstring();
    std::wstring dir = cur.parent_path().wstring();
    wchar_t file[MAX_PATH]; wcsncpy_s(file, stem.c_str(), _TRUNCATE);
    OPENFILENAMEW o{ sizeof(o) };
    o.hwndOwner = g.hwnd;
    o.lpstrFilter = g.hdr ? L"JPEG XR (HDR)\0*.jxr\0PNG (SDR)\0*.png\0JPEG (SDR)\0*.jpg\0\0"
                          : L"PNG\0*.png\0JPEG\0*.jpg\0JPEG XR\0*.jxr\0BMP\0*.bmp\0TIFF\0*.tif\0\0";
    o.lpstrFile = file; o.nMaxFile = MAX_PATH;
    o.lpstrInitialDir = dir.c_str();
    o.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (!GetSaveFileNameW(&o)) return false;
    fs::path out = file;
    if (out.extension().empty()) {
        static const wchar_t* hdrExt[] = { L".jxr", L".png", L".jpg" };
        static const wchar_t* sdrExt[] = { L".png", L".jpg", L".jxr", L".bmp", L".tif" };
        int i = (int)o.nFilterIndex - 1;
        out += g.hdr ? hdrExt[std::clamp(i, 0, 2)] : sdrExt[std::clamp(i, 0, 4)];
    }
    if (!WriteImage(out)) return false;
    ReopenKeepHistory(out);
    return true;
}

static bool DoSave(bool alreadyConfirmed = false) {
    if (!g.srv || g.files.empty()) return false;
    if (g.ops.empty() && !g.adjustDirty && !g.resizeDirty) return true;
    fs::path p = g.files[g.idx];
    std::wstring e = LowerExt(p);
    bool ok = e == L".jxr" || (!g.hdr && (e == L".png" || e == L".jpg" || e == L".jpeg" || e == L".bmp" ||
                                         e == L".tif" || e == L".tiff"));
    if (!ok) return DoSaveAs();
    if (!alreadyConfirmed && RunConfirmDialog(1) != 1) return false;

    // Guardado seguro al sobrescribir el archivo original:
    // primero escribimos una copia temporal y solo sustituimos el original
    // cuando la codificación ha terminado correctamente. Esto evita que
    // WIC falle al abrir el mismo archivo que acabamos de cargar y además
    // protege el original si la escritura se interrumpe.
    std::wstring base = p.wstring();
    std::wstring tmpName = base + L".pikviewer_tmp" + p.extension().wstring();
    fs::path tmp(tmpName);
    std::error_code ec;
    fs::remove(tmp, ec);

    if (!WriteImage(tmp)) {
        fs::remove(tmp, ec);
        MessageBoxW(g.hwnd, T(L"No se pudo guardar la imagen."), L"PikViewer", MB_ICONWARNING);
        return false;
    }

    // Intentar reemplazo atómico; si falla (archivo bloqueado, OneDrive, etc.),
    // borrar el original y renombrar el temporal.
    if (!MoveFileExW(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        if (!DeleteFileW(p.c_str()) || !MoveFileW(tmp.c_str(), p.c_str())) {
            DWORD err = GetLastError();
            fs::remove(tmp, ec);
            wchar_t msg[256];
            swprintf_s(msg, L"%s\n\nCódigo de error: %lu", T(L"No se pudo reemplazar el archivo original."), (unsigned long)err);
            MessageBoxW(g.hwnd, msg, L"PikViewer", MB_ICONWARNING);
            return false;
        }
    }

    if (alreadyConfirmed) OpenPath(p); // se va a cerrar o cambiar de imagen: no hace falta conservar el historial
    else ReopenKeepHistory(p);
    return true;
}

static bool ConfirmDiscard() {
    if (g.ops.empty() && !g.adjustDirty && !g.resizeDirty) return true;
    int r = RunConfirmDialog(2);
    if (r == 3 || r == 0) return false;
    if (r == 1) return DoSave(true);
    return true;
}

// ------------------------------------------------------------------ archivos / navegacion
static void Navigate(int d) {
    int n = (int)g.files.size();
    if (n < 2) return;
    if (!ConfirmDiscard()) return;
    for (int t = 0; t < n; t++) {
        g.idx = (g.idx + d + n) % n;
        if (LoadPicture(g.files[g.idx])) { Render(); return; }
    }
}

static void OpenPath(fs::path p) {
    std::error_code ec;
    p = fs::absolute(p, ec);
    g.files.clear();
    try {
        for (auto& e : fs::directory_iterator(p.parent_path(), ec))
            if (e.is_regular_file(ec) && IsImage(e.path())) g.files.push_back(e.path());
    } catch (...) {}
    std::sort(g.files.begin(), g.files.end(), [](const fs::path& a, const fs::path& b) {
        return StrCmpLogicalW(a.filename().c_str(), b.filename().c_str()) < 0; });
    g.idx = -1;
    for (int i = 0; i < (int)g.files.size(); i++)
        if (_wcsicmp(g.files[i].filename().c_str(), p.filename().c_str()) == 0) { g.idx = i; break; }
    if (g.idx < 0) { g.files.push_back(p); g.idx = (int)g.files.size() - 1; }
    UpdateWhite();
    if (!LoadPicture(g.files[g.idx])) {
        MessageBoxW(g.hwnd, T(L"No se pudo abrir la imagen."), L"PikViewer", MB_ICONWARNING);
        return;
    }
    Render();
}

static void DoOpen() {
    if (!ConfirmDiscard()) return;
    wchar_t file[MAX_PATH] = L"";
    OPENFILENAMEW o{ sizeof(o) };
    o.hwndOwner = g.hwnd; o.lpstrFilter = kOpenFilter;
    o.lpstrFile = file; o.nMaxFile = MAX_PATH;
    o.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameW(&o)) OpenPath(file);
}

// ------------------------------------------------------------------ edicion: historial y busqueda
static void RefreshAdjustDirty();

// Copia la textura base actual a un snapshot en GPU (para deshacer redimensionados).
static ComPtr<ID3D11Texture2D> SnapshotBase() {
    if (!g.base) return nullptr;
    D3D11_TEXTURE2D_DESC td{};
    g.base->GetDesc(&td);
    td.MipLevels = 1; td.MiscFlags = 0;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.CPUAccessFlags = 0;
    ComPtr<ID3D11Texture2D> snap;
    if (FAILED(g.dev->CreateTexture2D(&td, nullptr, &snap))) return nullptr;
    g.ctx->CopyResource(snap.Get(), g.base.Get());
    return snap;
}

// Restaura base/work/disp/srv/workBmp a partir de un snapshot de tamaño w×h.
static bool RestoreFromSnapshot(ID3D11Texture2D* snap, UINT w, UINT h) {
    if (!snap || !w || !h) return false;
    DXGI_FORMAT fmt = g.hdr ? DXGI_FORMAT_R16G16B16A16_FLOAT
                    : g.deep ? DXGI_FORMAT_R16G16B16A16_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;

    D3D11_TEXTURE2D_DESC td{};
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1; td.Format = fmt;
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> base, work, disp;
    if (FAILED(g.dev->CreateTexture2D(&td, nullptr, &base))) return false;
    g.ctx->CopyResource(base.Get(), snap);

    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    if (FAILED(g.dev->CreateTexture2D(&td, nullptr, &work))) return false;
    td.MipLevels = 0; td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
    if (FAILED(g.dev->CreateTexture2D(&td, nullptr, &disp))) return false;
    ComPtr<ID3D11ShaderResourceView> sv;
    if (FAILED(g.dev->CreateShaderResourceView(disp.Get(), nullptr, &sv))) return false;

    ComPtr<ID2D1Bitmap1> wb;
    {
        ComPtr<IDXGISurface> ws; work.As(&ws);
        D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(fmt, g.hdr ? D2D1_ALPHA_MODE_PREMULTIPLIED : D2D1_ALPHA_MODE_IGNORE), 96.f, 96.f);
        if (FAILED(g.d2d->CreateBitmapFromDxgiSurface(ws.Get(), &bp, &wb))) wb.Reset();
    }

    g.base = base; g.work = work; g.disp = disp; g.srv = sv; g.workBmp = wb;
    g.imgW = w; g.imgH = h;
    g.canEdit = (wb != nullptr);
    return true;
}

static App::Hist CaptureHist(bool withImage) {
    App::Hist h;
    h.ops = g.ops;
    h.gamma = g.gamma; h.brightness = g.brightness;
    h.contrast = g.contrast; h.saturation = g.saturation;
    h.resizeDirty = g.resizeDirty; h.resizeW = g.resizeW; h.resizeH = g.resizeH;
    h.hdr = g.hdr; h.deep = g.deep;
    if (withImage && g.base) {
        h.hasImg = true;
        h.imgW = g.imgW; h.imgH = g.imgH;
        h.info = g.info;
        h.baseSnap = SnapshotBase();
        if (!h.baseSnap) h.hasImg = false;
    }
    return h;
}

static void ApplyHist(const App::Hist& h) {
    if (h.hasImg && h.baseSnap) {
        // RestoreFromSnapshot crea las texturas segun g.hdr/g.deep: deben coincidir con las del snapshot.
        const bool oldHdr = g.hdr, oldDeep = g.deep;
        g.hdr = h.hdr; g.deep = h.deep;
        if (RestoreFromSnapshot(h.baseSnap.Get(), h.imgW, h.imgH)) g.info = h.info;
        else { g.hdr = oldHdr; g.deep = oldDeep; }
        g.resizeW = h.resizeW; g.resizeH = h.resizeH;
        g.resizeWText = std::to_wstring(h.resizeW > 0 ? h.resizeW : (int)h.imgW);
        g.resizeHText = std::to_wstring(h.resizeH > 0 ? h.resizeH : (int)h.imgH);
        g.resizeAspect = h.imgH ? (float)h.imgW / (float)h.imgH : 1.f;
        g.resizeDirty = h.resizeDirty;
        g.cropping = false; g.cropHandle = 0; g.cropBoxChanged = false;
        g.zoom = 1.0f; g.panX = 0.0f; g.panY = 0.0f;
    }
    g.ops = h.ops;
    g.gamma = h.gamma; g.brightness = h.brightness;
    g.contrast = h.contrast; g.saturation = h.saturation;
    RefreshAdjustDirty();
    Redraw(nullptr);
}

static void PushUndo(bool withImage = false) {
    g.undoS.push_back(CaptureHist(withImage));
    g.redoS.clear();
}

static void DoUndo() {
    if (g.undoS.empty()) return;
    g.redoS.push_back(CaptureHist(g.undoS.back().hasImg));
    App::Hist h = std::move(g.undoS.back()); g.undoS.pop_back();
    ApplyHist(h);
    UpdateTitle(); Render();
}
static void DoRedo() {
    if (g.redoS.empty()) return;
    g.undoS.push_back(CaptureHist(g.redoS.back().hasImg));
    App::Hist h = std::move(g.redoS.back()); g.redoS.pop_back();
    ApplyHist(h);
    UpdateTitle(); Render();
}

static float DistSeg(P p, P a, P b) {
    float dx = b.x - a.x, dy = b.y - a.y, l2 = dx * dx + dy * dy;
    float t = l2 > 0 ? ((p.x - a.x) * dx + (p.y - a.y) * dy) / l2 : 0.f;
    t = std::clamp(t, 0.f, 1.f);
    float ex = a.x + t * dx - p.x, ey = a.y + t * dy - p.y;
    return sqrtf(ex * ex + ey * ey);
}

static bool HitOp(const Op& o, P p, float tol) {
    float r = tol + o.width * 0.5f;
    switch (o.type) {
    case T_PEN: case T_HIGH:
        if (o.pts.size() == 1) return DistSeg(p, o.pts[0], o.pts[0]) <= r;
        for (size_t i = 0; i + 1 < o.pts.size(); i++) if (DistSeg(p, o.pts[i], o.pts[i + 1]) <= r) return true;
        return false;
    case T_LINE: case T_ARROW: return DistSeg(p, o.pts[0], o.pts[1]) <= r;
    case T_RECT: {
        D2D1_RECT_F q = Norm(o.pts[0], o.pts[1]);
        P a{ q.left, q.top }, b{ q.right, q.top }, c{ q.right, q.bottom }, d{ q.left, q.bottom };
        return DistSeg(p, a, b) <= r || DistSeg(p, b, c) <= r || DistSeg(p, c, d) <= r || DistSeg(p, d, a) <= r;
    }
    case T_ELL: {
        D2D1_RECT_F q = Norm(o.pts[0], o.pts[1]);
        float cx = (q.left + q.right) / 2, cy = (q.top + q.bottom) / 2;
        float rx = std::max(0.5f, (q.right - q.left) / 2), ry = std::max(0.5f, (q.bottom - q.top) / 2);
        float nx = (p.x - cx) / rx, ny = (p.y - cy) / ry;
        return fabsf(sqrtf(nx * nx + ny * ny) - 1.f) * std::min(rx, ry) <= r;
    }
    }
    return false;
}

static void EraseAt(P p, float f) {
    // La goma solo afecta a las anotaciones dibujadas encima de la imagen.
    // Nunca modifica los píxeles de la imagen base ni los recortes.
    float tol = 8.f * Dpi() / std::max(0.0001f, f);
    bool changed = false;

    for (int i = (int)g.ops.size() - 1; i >= 0; --i) {
        Op& o = g.ops[i];
        if (o.type == T_CROP || o.pts.empty()) continue;

        // Boli y rotulador: borrar solo la parte del trazo que toca la goma.
        if (o.type == T_PEN || o.type == T_HIGH) {
            if (!HitOp(o, p, tol)) continue;

            std::vector<Op> pieces;
            std::vector<P> run;
            auto flushRun = [&]() {
                if (run.empty()) return;
                Op piece = o;
                piece.pts = run;
                if (piece.pts.size() >= 2 || o.pts.size() == 1)
                    pieces.push_back(std::move(piece));
                run.clear();
            };

            for (const P& q : o.pts) {
                bool erasePoint = DistSeg(p, q, q) <= tol + o.width * 0.5f;
                if (erasePoint) flushRun();
                else run.push_back(q);
            }
            flushRun();

            // Si el cursor cruza un segmento entre dos puntos sin tocar los
            // puntos almacenados, abrimos el trazo por el punto más cercano.
            if (pieces.size() == 1 && pieces[0].pts.size() == o.pts.size() && o.pts.size() > 1) {
                size_t best = 0;
                float bestD = FLT_MAX;
                for (size_t k = 0; k + 1 < o.pts.size(); ++k) {
                    float d = DistSeg(p, o.pts[k], o.pts[k + 1]);
                    if (d < bestD) { bestD = d; best = k + 1; }
                }
                if (bestD <= tol + o.width * 0.5f) {
                    pieces.clear();
                    Op a = o, b = o;
                    a.pts.assign(o.pts.begin(), o.pts.begin() + best);
                    b.pts.assign(o.pts.begin() + best, o.pts.end());
                    if (a.pts.size() >= 2) pieces.push_back(std::move(a));
                    if (b.pts.size() >= 2) pieces.push_back(std::move(b));
                }
            }

            if (pieces.size() == 1 && pieces[0].pts.size() == o.pts.size()) continue;

            if (!g.erased) { PushUndo(); g.erased = true; }
            g.ops.erase(g.ops.begin() + i);
            g.ops.insert(g.ops.begin() + i,
                         std::make_move_iterator(pieces.begin()),
                         std::make_move_iterator(pieces.end()));
            changed = true;
            continue;
        }

        // Formas, líneas y flechas: al tocarlas se elimina esa anotación
        // completa, pero la imagen original permanece intacta.
        if (HitOp(o, p, tol)) {
            if (!g.erased) { PushUndo(); g.erased = true; }
            g.ops.erase(g.ops.begin() + i);
            changed = true;
        }
    }

    if (changed) {
        Redraw(nullptr);
        UpdateTitle();
        Render();
    }
}

// ------------------------------------------------------------------ UI: layout y dibujo
static void Layout(float W, float S) {
    g.btns.clear(); g.seps.clear();
    float bw = 40 * S, bh = 40 * S, y0 = 4 * S, gap = 4 * S, sep = 14 * S;
    auto add = [&](int id, float x, float w) { g.btns.push_back({ id, { x, y0, x + w, y0 + bh } }); };
    add(B_FILE, 10 * S, 76 * S);
    const int ids[] = { B_PEN, B_HIGH, B_ERASE, B_SHAPE, B_CROP, B_CROP_BOX, B_UNDO, B_REDO, B_COLOR, B_WIDTH, B_ADJUST };
    float total = 10 * bw + 72 * S + 10 * gap + 3 * (sep - gap);
    float x = std::max(96 * S, (W - total) / 2);
    for (int id : ids) {
        if (id == B_CROP || id == B_UNDO || id == B_COLOR) { g.seps.push_back(x - gap + (sep - gap) / 2 + gap / 2); x += sep - gap; }
        float ww = (id == B_ADJUST) ? 72 * S : bw;
        add(id, x, ww);
        x += ww + gap;
    }
    add(B_SAVE, W - 10 * S - bw, bw);
    add(B_INFO, W - 10 * S - 2 * bw - gap, bw);
}

static void DrawGlyph(const wchar_t* gl, D2D1_RECT_F r, uint32_t col) {
    if (!g.fIcon) return;
    g.brush->SetColor(UiColor(col));
    g.d2d->DrawText(gl, 1, g.fIcon.Get(), r, g.brush.Get());
}

static void DrawButton(const Button& b, float S) {
    bool active = false, enabled = true;
    switch (b.id) {
    case B_PEN: active = g.tool == T_PEN; break;
    case B_HIGH: active = g.tool == T_HIGH; break;
    case B_ERASE: active = g.tool == T_ERASE; break;
    case B_SHAPE: active = g.tool >= T_RECT && g.tool <= T_ARROW; break;
    case B_CROP: active = g.tool == T_CROP; break;
    case B_CROP_BOX: active = g.tool == T_CROP_BOX; break;
    case B_INFO: active = g.showInfo; break;
    case B_ADJUST: active = g.showAdjust; break;
    case B_UNDO: enabled = !g.undoS.empty(); break;
    case B_REDO: enabled = !g.redoS.empty(); break;
    case B_SAVE: enabled = !g.ops.empty() || g.adjustDirty || g.resizeDirty; break;
    }
    D2D1_RECT_F r = b.r;
    D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(r, 6 * S, 6 * S);
    if (active) {
        g.brush->SetColor(UiColor(0x4CC2FF, 0.18f)); g.d2d->FillRoundedRectangle(rr, g.brush.Get());
        g.brush->SetColor(UiColor(0x4CC2FF, 0.9f)); g.d2d->DrawRoundedRectangle(rr, g.brush.Get(), 1.2f * S);
    } else if (g.hoverBtn == b.id && enabled) {
        g.brush->SetColor(UiColor(0xFFFFFF, 0.09f)); g.d2d->FillRoundedRectangle(rr, g.brush.Get());
    }
    uint32_t col = enabled ? 0xF2F2F2 : 0x5E646B;
    float cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
    switch (b.id) {
    case B_FILE: {
        D2D1_RECT_F t{ r.left, r.top, r.right - 16 * S, r.bottom };
        g.brush->SetColor(UiColor(0xF2F2F2));
        if (g.fCenter) g.d2d->DrawText(T(L"Archivo"), (UINT32)wcslen(T(L"Archivo")), g.fCenter.Get(), t, g.brush.Get());
        float ax = r.right - 15 * S;
        g.d2d->DrawLine(P{ ax - 3.5f * S, cy - 1.5f * S }, P{ ax, cy + 2 * S }, g.brush.Get(), 1.4f * S, g.ssRound.Get());
        g.d2d->DrawLine(P{ ax, cy + 2 * S }, P{ ax + 3.5f * S, cy - 1.5f * S }, g.brush.Get(), 1.4f * S, g.ssRound.Get());
        break;
    }
    case B_PEN: DrawGlyph(L"\uE70F", r, col); break;
    case B_HIGH: DrawGlyph(L"\uE7E6", r, col); break;
    case B_ERASE: DrawGlyph(L"\uE75C", r, col); break;
    case B_CROP: DrawGlyph(L"\uE7A8", r, col); break;
    case B_CROP_BOX: {
        g.brush->SetColor(UiColor(col));
        float m = 14*S;
        D2D1_RECT_F q = D2D1::RectF(r.left+m, r.top+m, r.right-m, r.bottom-m);
        g.d2d->DrawRectangle(q, g.brush.Get(), 1.5f*S);
        float h=2.0f*S;
        for (float px : {q.left,q.right}) for (float py : {q.top,q.bottom})
            g.d2d->FillEllipse(D2D1::Ellipse(P{px,py},h,h), g.brush.Get());
        break;
    }
    case B_UNDO: DrawGlyph(L"\uE7A7", r, col); break;
    case B_REDO: DrawGlyph(L"\uE7A6", r, col); break;
    case B_SAVE: DrawGlyph(L"\uE74E", r, col); break;
    case B_INFO: DrawGlyph(L"\uE946", r, col); break;
    case B_SHAPE: {
        g.brush->SetColor(UiColor(col));
        g.d2d->DrawRectangle(D2D1::RectF(cx - 9 * S, cy - 8 * S, cx + 3 * S, cy + 3 * S), g.brush.Get(), 1.6f * S);
        g.d2d->DrawEllipse(D2D1::Ellipse(P{ cx + 3 * S, cy + 3 * S }, 6 * S, 6 * S), g.brush.Get(), 1.6f * S);
        break;
    }
    case B_COLOR: {
        g.brush->SetColor(UiColor(g.color));
        g.d2d->FillEllipse(D2D1::Ellipse(P{ cx, cy }, 8 * S, 8 * S), g.brush.Get());
        g.brush->SetColor(UiColor(0xFFFFFF, 0.5f));
        g.d2d->DrawEllipse(D2D1::Ellipse(P{ cx, cy }, 9 * S, 9 * S), g.brush.Get(), 1.2f * S);
        break;
    }
    case B_ADJUST: {
        g.brush->SetColor(UiColor(col));
        D2D1_RECT_F t{ r.left + 7*S, r.top, r.right - 14*S, r.bottom };
        g.d2d->DrawText(T(L"Ajustes"), (UINT32)wcslen(T(L"Ajustes")), g.fCenter.Get(), t, g.brush.Get());
        float ax = r.right - 10*S;
        g.d2d->DrawLine(P{ax-3*S,cy-1.5f*S},P{ax,cy+2*S},g.brush.Get(),1.4f*S,g.ssRound.Get());
        g.d2d->DrawLine(P{ax,cy+2*S},P{ax+3*S,cy-1.5f*S},g.brush.Get(),1.4f*S,g.ssRound.Get());
        break;
    }
    case B_WIDTH: {
        g.brush->SetColor(UiColor(col));
        float ws[3] = { 1.2f, 2.6f, 4.2f };
        for (int i = 0; i < 3; i++) {
            float yy = cy + (i - 1) * 6.5f * S;
            g.d2d->DrawLine(P{ cx - 9 * S, yy }, P{ cx + 9 * S, yy }, g.brush.Get(), ws[i] * S);
        }
        break;
    }
    }
}

static void DrawChevron(float cx, float cy, int dir, float S) {
    P a{ cx - dir * 4 * S, cy - 11 * S }, b{ cx + dir * 4 * S, cy }, c{ cx - dir * 4 * S, cy + 11 * S };
    g.brush->SetColor(UiColor(0x000000, 0.5f));
    for (P* q : { &a, &c }) g.d2d->DrawLine(P{ q->x, q->y + S }, P{ b.x, b.y + S }, g.brush.Get(), 3.4f * S, g.ssRound.Get());
    g.brush->SetColor(UiColor(0xFFFFFF, 0.95f));
    g.d2d->DrawLine(a, b, g.brush.Get(), 2.2f * S, g.ssRound.Get());
    g.d2d->DrawLine(b, c, g.brush.Get(), 2.2f * S, g.ssRound.Get());
}

static std::vector<std::pair<std::wstring, std::wstring>> InfoRows() {
    static const wchar_t* names[] = { T(L"sRGB por tramos"), L"Gamma 2.2", L"Gamma 2.4" };
    std::vector<std::pair<std::wstring, std::wstring>> r;
    if (g.idx < 0 || g.idx >= (int)g.files.size()) return r;
    r.push_back({ T(L"Archivo"), g.files[g.idx].filename().wstring() });
    r.push_back({ T(L"Posición"), Fmt(L"%d de %d", g.idx + 1, (int)g.files.size()) });
    r.push_back({ T(L"Dimensiones"), Fmt(L"%u × %u px", g.imgW, g.imgH) });
    CR c = CropRect();
    if ((UINT)c.w != g.imgW || (UINT)c.h != g.imgH) r.push_back({ T(L"Recorte"), Fmt(L"%.0f × %.0f px", c.w, c.h) });
    double mb = g.info.fsize / 1048576.0;
    r.push_back({ T(L"Tamaño"), mb >= 1 ? Fmt(L"%.2f MB", mb) : Fmt(L"%.1f KB", g.info.fsize / 1024.0) });
    r.push_back({ T(L"Formato"), g.info.container });
    r.push_back({ T(L"Píxel"), g.info.pixfmt });
    r.push_back({ T(L"Rango"), g.hdr ? L"HDR (lineal)" : (g.deep ? L"SDR (16 bits)" : L"SDR (8 bits)") });
    r.push_back({ T(L"Espacio de color"), g.info.cs });
    if (g.hdr) {
        r.push_back({ T(L"Brillo máximo"), Fmt(L"%.0f nits", g.info.maxN) });
        r.push_back({ T(L"Brillo medio"), Fmt(L"%.1f nits", g.info.avgN) });
    } else r.push_back({ T(L"Curva (tecla E)"), names[g.eotf] });
    r.push_back({ T(L"Blanco SDR del sistema"), Fmt(L"%.0f nits", g.white * 80.f) });
    return r;
}

static int ParsePositiveInt(const std::wstring& s) {
    if (s.empty()) return 0;
    long long v = 0;
    for (wchar_t c : s) {
        if (c < L'0' || c > L'9') return 0;
        v = v * 10 + (c - L'0');
        if (v > 16384) return 0;
    }
    return (int)v;
}

static void SyncResizeFieldsFromCrop() {
    CR c = CropRect();
    int w = std::max(1, (int)std::lround(c.w));
    int h = std::max(1, (int)std::lround(c.h));
    g.resizeW = w; g.resizeH = h;
    g.resizeWText = std::to_wstring(w); g.resizeHText = std::to_wstring(h);
    g.resizeAspect = (float)w / (float)h;
}

static void UpdateResizeAspectFromField() {
    int w = ParsePositiveInt(g.resizeWText), h = ParsePositiveInt(g.resizeHText);
    if (!g.keepAspect || g.resizeAspect <= 0) return;
    if (g.resizeField == 1 && w > 0) {
        h = std::max(1, (int)std::lround(w / g.resizeAspect));
        g.resizeHText = std::to_wstring(h);
    } else if (g.resizeField == 2 && h > 0) {
        w = std::max(1, (int)std::lround(h * g.resizeAspect));
        g.resizeWText = std::to_wstring(w);
    }
}

static void ApplyResize() {
    int w = ParsePositiveInt(g.resizeWText), h = ParsePositiveInt(g.resizeHText);
    if (w < 1 || h < 1 || w > 16384 || h > 16384 || !g.work) return;
    CR c = CropRect();
    int ow = std::max(1, (int)std::lround(c.w)), oh = std::max(1, (int)std::lround(c.h));
    g.resizeField = 0; g.resizeSelectAll = false;
    KillCaretTimer();
    g.showAdjust = false; // cerrar el panel al aplicar
    if (w == ow && h == oh) {
        g.resizeW = w; g.resizeH = h;
        UpdateTitle(); Render();
        return;
    }

    // Guardar estado actual (imagen + ops + ajustes) para poder deshacer.
    PushUndo(true);

    // Extraer la región actual (recorte o imagen completa) con anotaciones ya dibujadas.
    // Misma ruta que WriteImage para no alterar colores.
    UINT cx = (UINT)std::max(0.f, c.x), cy = (UINT)std::max(0.f, c.y);
    UINT cw = (UINT)ow, ch = (UINT)oh;
    if (cx + cw > g.imgW) cw = g.imgW - cx;
    if (cy + ch > g.imgH) ch = g.imgH - cy;
    if (!cw || !ch) return;

    DXGI_FORMAT fmt = g.hdr ? DXGI_FORMAT_R16G16B16A16_FLOAT
                    : g.deep ? DXGI_FORMAT_R16G16B16A16_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
    UINT bpp = (g.hdr || g.deep) ? 8 : 4;
    WICPixelFormatGUID sf = g.hdr ? GUID_WICPixelFormat64bppRGBAHalf
                          : g.deep ? GUID_WICPixelFormat64bppRGBA : GUID_WICPixelFormat32bppRGBA;

    D3D11_TEXTURE2D_DESC d{};
    g.work->GetDesc(&d);
    d.Width = cw; d.Height = ch; d.MipLevels = 1; d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> st;
    if (FAILED(g.dev->CreateTexture2D(&d, nullptr, &st))) return;
    D3D11_BOX box{ cx, cy, 0, cx + cw, cy + ch, 1 };
    g.ctx->CopySubresourceRegion(st.Get(), 0, 0, 0, 0, g.work.Get(), 0, &box);
    D3D11_MAPPED_SUBRESOURCE ms{};
    if (FAILED(g.ctx->Map(st.Get(), 0, D3D11_MAP_READ, 0, &ms))) return;

    // Crear bitmap WIC directamente desde el staging (igual que al guardar).
    ComPtr<IWICBitmap> bmp;
    HRESULT hr = g.wic->CreateBitmapFromMemory(cw, ch, sf, ms.RowPitch,
        ms.RowPitch * (ch - 1) + cw * bpp, (BYTE*)ms.pData, &bmp);
    g.ctx->Unmap(st.Get(), 0);
    st.Reset();
    if (FAILED(hr)) return;

    // Escalar con Fant (misma calidad que al guardar).
    ComPtr<IWICBitmapScaler> scaler;
    if (FAILED(g.wic->CreateBitmapScaler(&scaler)) ||
        FAILED(scaler->Initialize(bmp.Get(), (UINT)w, (UINT)h, WICBitmapInterpolationModeFant))) {
        MessageBoxW(g.hwnd, T(L"No se pudo redimensionar la imagen."), L"PikViewer", MB_ICONWARNING);
        return;
    }

    // Forzar el formato de píxel original por si el scaler cambió el GUID interno.
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(g.wic->CreateFormatConverter(&conv)) ||
        FAILED(conv->Initialize(scaler.Get(), sf, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
        MessageBoxW(g.hwnd, T(L"No se pudo redimensionar la imagen."), L"PikViewer", MB_ICONWARNING);
        return;
    }

    UINT stride = (UINT)w * bpp;
    std::vector<BYTE> dstBuf((size_t)stride * h);
    if (FAILED(conv->CopyPixels(nullptr, stride, (UINT)dstBuf.size(), dstBuf.data()))) return;

    // Crear nuevas texturas al tamaño destino (misma estructura que LoadPicture).
    D3D11_TEXTURE2D_DESC td{};
    td.Width = (UINT)w; td.Height = (UINT)h; td.MipLevels = 1; td.ArraySize = 1; td.Format = fmt;
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd{ dstBuf.data(), stride, 0 };
    ComPtr<ID3D11Texture2D> base, work, disp;
    if (FAILED(g.dev->CreateTexture2D(&td, &sd, &base))) return;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    if (FAILED(g.dev->CreateTexture2D(&td, nullptr, &work))) return;
    td.MipLevels = 0; td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
    if (FAILED(g.dev->CreateTexture2D(&td, nullptr, &disp))) return;
    ComPtr<ID3D11ShaderResourceView> sv;
    if (FAILED(g.dev->CreateShaderResourceView(disp.Get(), nullptr, &sv))) return;

    ComPtr<ID2D1Bitmap1> wb;
    {
        ComPtr<IDXGISurface> ws; work.As(&ws);
        D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(fmt, g.hdr ? D2D1_ALPHA_MODE_PREMULTIPLIED : D2D1_ALPHA_MODE_IGNORE), 96.f, 96.f);
        if (FAILED(g.d2d->CreateBitmapFromDxgiSurface(ws.Get(), &bp, &wb))) wb.Reset();
    }

    g.base = base; g.work = work; g.disp = disp; g.srv = sv; g.workBmp = wb;
    g.imgW = (UINT)w; g.imgH = (UINT)h;
    g.canEdit = (wb != nullptr);
    g.ops.clear(); // las anotaciones ya están horneadas en el snapshot anterior
    g.cropping = false; g.drawing = false; g.cropHandle = 0; g.cropBoxChanged = false;
    g.tool = T_NONE;
    g.zoom = 1.0f; g.panX = 0.0f; g.panY = 0.0f; g.panning = false;
    g.resizeW = w; g.resizeH = h;
    g.resizeWText = std::to_wstring(w); g.resizeHText = std::to_wstring(h);
    g.resizeAspect = (float)w / (float)h;
    g.resizeDirty = true;
    Redraw(nullptr);
    UpdateTitle();
    Render();
}

static D2D1_RECT_F ResizeWidthBox(const D2D1_RECT_F& p, float S) { return D2D1::RectF(p.left + 18*S, p.top + 305*S, p.left + 138*S, p.top + 337*S); }
static D2D1_RECT_F ResizeHeightBox(const D2D1_RECT_F& p, float S) { return D2D1::RectF(p.left + 150*S, p.top + 305*S, p.left + 270*S, p.top + 337*S); }
static D2D1_RECT_F ResizeKeepBox(const D2D1_RECT_F& p, float S) { return D2D1::RectF(p.left + 18*S, p.top + 344*S, p.left + 32*S, p.top + 358*S); }
static D2D1_RECT_F ResizeApplyBox(const D2D1_RECT_F& p, float S) { return D2D1::RectF(p.right - 126*S, p.top + 342*S, p.right - 18*S, p.top + 368*S); }

static void DrawAdjustPanel(float W) {
    if (!g.showAdjust) return;
    float S = Dpi(); auto p = AdjustPanelRect(W);
    g.brush->SetColor(UiColor(0x17191D,0.97f));
    g.d2d->FillRoundedRectangle(D2D1::RoundedRect(p,10*S,10*S),g.brush.Get());
    g.brush->SetColor(UiColor(0x44484F,0.95f));
    g.d2d->DrawRoundedRectangle(D2D1::RoundedRect(p,10*S,10*S),g.brush.Get(),1*S);
    g.brush->SetColor(UiColor(0xF2F2F2));
    g.d2d->DrawText(T(L"Ajustes de imagen"),(UINT32)wcslen(T(L"Ajustes de imagen")),g.fLeft.Get(),
        D2D1::RectF(p.left+18*S,p.top+12*S,p.right-18*S,p.top+40*S),g.brush.Get());

    struct A { const wchar_t* name; float val; };
    A a[] = {
        {L"Gamma",g.gamma},
        {T(L"Brillo"),g.brightness},
        {T(L"Contraste"),g.contrast},
        {T(L"Saturación"),g.saturation}
    };

    const AdjustGeo A = GetAdjustGeo(p, S);
    const float x0 = A.x0, sx = A.sx, resetL = A.resetL, valueL = A.valueL, ex = A.ex, start = A.start, row = A.row;

    for (int i=0; i<4; ++i) {
        float yy = start + i*row;
        float ty = yy - 12*S;
        float t = std::clamp((a[i].val + 1.f) * 0.5f, 0.f, 1.f);
        float k = sx + t*(ex-sx);

        g.brush->SetColor(UiColor(0xD9DDE2));
        g.d2d->DrawText(a[i].name,(UINT32)wcslen(a[i].name),g.fLeft.Get(),
            D2D1::RectF(x0,ty,x0+72*S,ty+24*S),g.brush.Get());

        g.brush->SetColor(UiColor(0x565B63,0.9f));
        g.d2d->DrawLine(P{sx,yy},P{ex,yy},g.brush.Get(),4*S,g.ssRound.Get());
        g.brush->SetColor(UiColor(0x4CC2FF,0.95f));
        g.d2d->DrawLine(P{sx+0.5f*(ex-sx),yy},P{k,yy},g.brush.Get(),4*S,g.ssRound.Get());
        g.d2d->FillEllipse(D2D1::Ellipse(P{k,yy},6*S,6*S),g.brush.Get());

        // Valor actual
        g.brush->SetColor(UiColor(0xF2F2F2));
        std::wstring v = Fmt(L"%+.2f", a[i].val);
        g.d2d->DrawText(v.c_str(),(UINT32)v.size(),g.fCenter.Get(),
            D2D1::RectF(valueL,ty,resetL-4*S,ty+24*S),g.brush.Get());

        // Botón circular discreto para restablecer a 0.
        float rr = 11*S;
        P rp{resetL + 12*S, yy};
        g.brush->SetColor(UiColor(0x30353A,1.f));
        g.d2d->FillEllipse(D2D1::Ellipse(rp,rr,rr),g.brush.Get());
        g.brush->SetColor(UiColor(0x555B62,1.f));
        g.d2d->DrawEllipse(D2D1::Ellipse(rp,rr,rr),g.brush.Get(),1*S);
        g.brush->SetColor(UiColor(0xF2F2F2));
        g.d2d->DrawText(L"↻",1,g.fCenter.Get(),D2D1::RectF(resetL+1*S,yy-10*S,resetL+23*S,yy+10*S),g.brush.Get());
    }

    // Redimensionado
    g.brush->SetColor(UiColor(0xF2F2F2));
    g.d2d->DrawText(T(L"Tamaño"),(UINT32)wcslen(T(L"Tamaño")),g.fLeft.Get(),
        D2D1::RectF(p.left+18*S,p.top+275*S,p.right-18*S,p.top+300*S),g.brush.Get());

    auto drawField = [&](D2D1_RECT_F r, const wchar_t* label, const std::wstring& value, bool active) {
        // Fondo: más claro y con tinte al enfocar para que se note de inmediato.
        g.brush->SetColor(UiColor(active ? 0x3A4550 : 0x2A2F34, 1.f));
        g.d2d->FillRoundedRectangle(D2D1::RoundedRect(r, 6*S, 6*S), g.brush.Get());
        // Borde: cian grueso + anillo exterior suave cuando está activo.
        if (active) {
            g.brush->SetColor(UiColor(0x4CC2FF, 0.28f));
            g.d2d->DrawRoundedRectangle(D2D1::RoundedRect(
                D2D1::RectF(r.left - 2.5f*S, r.top - 2.5f*S, r.right + 2.5f*S, r.bottom + 2.5f*S),
                8*S, 8*S), g.brush.Get(), 2.5f*S);
            g.brush->SetColor(UiColor(0x4CC2FF, 1.f));
            g.d2d->DrawRoundedRectangle(D2D1::RoundedRect(r, 6*S, 6*S), g.brush.Get(), 2.0f*S);
        } else {
            g.brush->SetColor(UiColor(0x555B62, 1.f));
            g.d2d->DrawRoundedRectangle(D2D1::RoundedRect(r, 6*S, 6*S), g.brush.Get(), 1.f*S);
        }
        // Etiqueta
        g.brush->SetColor(UiColor(active ? 0xB8C4D0 : 0x9DA3AA));
        g.d2d->DrawText(label, (UINT32)wcslen(label), g.fLeft.Get(),
            D2D1::RectF(r.left + 8*S, r.top + 3*S, r.left + 48*S, r.bottom), g.brush.Get());

        // Área del valor numérico
        float vx0 = r.left + 48*S, vy0 = r.top + 3*S, vx1 = r.right - 8*S, vy1 = r.bottom - 3*S;
        // Resaltado de selección (cuando se acaba de hacer clic / Ctrl+A)
        if (active && g.resizeSelectAll && !value.empty()) {
            g.brush->SetColor(UiColor(0x4CC2FF, 0.35f));
            g.d2d->FillRoundedRectangle(D2D1::RoundedRect(
                D2D1::RectF(vx0 - 2*S, vy0 + 1*S, vx1 + 2*S, vy1 - 1*S), 3*S, 3*S), g.brush.Get());
        }
        g.brush->SetColor(UiColor(0xF2F2F2));
        g.d2d->DrawText(value.c_str(), (UINT32)value.size(), g.fLeft.Get(),
            D2D1::RectF(vx0, vy0, vx1, vy1), g.brush.Get());

        // Caret parpadeante al final del texto (solo si no hay selección completa)
        if (active && !g.resizeSelectAll) {
            bool on = ((GetTickCount() / 530) % 2) == 0;
            if (on) {
                // Medir anchura del texto para colocar el cursor.
                ComPtr<IDWriteTextLayout> layout;
                if (SUCCEEDED(g.dw->CreateTextLayout(value.c_str(), (UINT32)value.size(),
                        g.fLeft.Get(), 1000.f, 40.f, &layout))) {
                    DWRITE_TEXT_METRICS tm{};
                    layout->GetMetrics(&tm);
                    float cx = vx0 + tm.widthIncludingTrailingWhitespace;
                    if (cx > vx1 - 2*S) cx = vx1 - 2*S;
                    g.brush->SetColor(UiColor(0x4CC2FF, 1.f));
                    g.d2d->DrawLine(P{cx, vy0 + 2*S}, P{cx, vy1 - 2*S}, g.brush.Get(), 1.6f*S);
                }
            }
        }
    };
    if (!g.resizeDirty && g.resizeField == 0) SyncResizeFieldsFromCrop();
    drawField(ResizeWidthBox(p,S), T(L"Ancho"), g.resizeWText, g.resizeField==1);
    drawField(ResizeHeightBox(p,S), T(L"Alto"), g.resizeHText, g.resizeField==2);

    D2D1_RECT_F kb = ResizeKeepBox(p,S);
    g.brush->SetColor(UiColor(g.keepAspect ? 0x4CC2FF : 0x30353A));
    g.d2d->FillRoundedRectangle(D2D1::RoundedRect(kb,3*S,3*S),g.brush.Get());
    g.brush->SetColor(UiColor(0x70767D));
    g.d2d->DrawRoundedRectangle(D2D1::RoundedRect(kb,3*S,3*S),g.brush.Get(),1*S);
    if (g.keepAspect) {
        g.brush->SetColor(UiColor(0xFFFFFF));
        g.d2d->DrawText(L"✓",1,g.fCenter.Get(),D2D1::RectF(kb.left-1*S,kb.top-2*S,kb.right+1*S,kb.bottom+2*S),g.brush.Get());
    }
    g.brush->SetColor(UiColor(0xD9DDE2));
    g.d2d->DrawText(T(L"Mantener proporción"),(UINT32)wcslen(T(L"Mantener proporción")),g.fLeft.Get(),
        D2D1::RectF(kb.right+8*S,kb.top-2*S,p.left+190*S,kb.bottom+4*S),g.brush.Get());

    D2D1_RECT_F ab = ResizeApplyBox(p,S);
    g.brush->SetColor(UiColor(0x4CC2FF,0.18f));
    g.d2d->FillRoundedRectangle(D2D1::RoundedRect(ab,5*S,5*S),g.brush.Get());
    g.brush->SetColor(UiColor(0x4CC2FF,0.9f));
    g.d2d->DrawRoundedRectangle(D2D1::RoundedRect(ab,5*S,5*S),g.brush.Get(),1*S);
    g.brush->SetColor(UiColor(0xF2F2F2));
    std::wstring applyText = T(L"Aplicar tamaño");
    g.d2d->DrawText(applyText.c_str(),(UINT32)applyText.size(),g.fCenter.Get(),ab,g.brush.Get());
}


// Diálogo modal (g.dialog: 1 = sobrescribir, 2 = cambios sin guardar): panel y botones.
struct DialogGeo { float px, py, pw, ph; D2D1_RECT_F b[3]; int n; };
static DialogGeo GetDialogGeo(float W, float H, float S) {
    DialogGeo d{};
    d.n = g.dialog == 1 ? 2 : 3;
    d.pw = std::min(560.0f * S, W - 40.0f * S);
    d.ph = (g.dialog == 1 ? 180.0f : 195.0f) * S;
    d.px = (W - d.pw) * 0.5f; d.py = (H - d.ph) * 0.5f;
    float bw = (g.dialog == 1 ? 110.0f : 118.0f) * S, bh = 38 * S, gap = 10 * S, by = d.py + d.ph - 54 * S;
    float gx = d.px + (d.pw - (d.n * bw + (d.n - 1) * gap)) * 0.5f;
    for (int i = 0; i < d.n; i++) { float l = gx + i * (bw + gap); d.b[i] = D2D1::RectF(l, by, l + bw, by + bh); }
    return d;
}

// Aviso "usar como visor predeterminado": panel y botones Sí / No.
struct PromptGeo { float px, py, pw, ph; D2D1_RECT_F yes, no; };
static PromptGeo GetPromptGeo(float W, float H, float S) {
    PromptGeo d{};
    d.pw = std::min(620.0f * S, W - 40.0f * S); d.ph = 275.0f * S;
    d.px = (W - d.pw) * 0.5f; d.py = (H - d.ph) * 0.5f;
    float bw = 110 * S, bh = 38 * S, gap = 10 * S, by = d.py + d.ph - 52 * S;
    d.yes = D2D1::RectF(d.px + d.pw - 2 * bw - gap - 28 * S, by, d.px + d.pw - bw - gap - 28 * S, by + bh);
    d.no  = D2D1::RectF(d.px + d.pw - bw - 28 * S, by, d.px + d.pw - 28 * S, by + bh);
    return d;
}

static void DrawUI(float W, float H) {
    if (!g.d2d || !g.bbBmp) return;
    float S = Dpi(), top = 48 * S;
    g.d2d->SetTarget(g.bbBmp.Get());
    g.d2d->BeginDraw();
    g.d2d->SetTransform(D2D1::Matrix3x2F::Identity());

    // overlay de recorte
    if (g.srv && (g.tool == T_CROP || g.tool == T_CROP_BOX) && g.cropping) {
        View v = GetView();
        float x0 = v.dx + (std::min(g.cropA.x, g.cropB.x) - v.c.x) * v.f, x1 = v.dx + (std::max(g.cropA.x, g.cropB.x) - v.c.x) * v.f;
        float y0 = v.dy + (std::min(g.cropA.y, g.cropB.y) - v.c.y) * v.f, y1 = v.dy + (std::max(g.cropA.y, g.cropB.y) - v.c.y) * v.f;
        g.brush->SetColor(UiColor(0x000000, 0.55f));
        g.d2d->FillRectangle(D2D1::RectF(v.dx, v.dy, v.dx + v.dw, y0), g.brush.Get());
        g.d2d->FillRectangle(D2D1::RectF(v.dx, y1, v.dx + v.dw, v.dy + v.dh), g.brush.Get());
        g.d2d->FillRectangle(D2D1::RectF(v.dx, y0, x0, y1), g.brush.Get());
        g.d2d->FillRectangle(D2D1::RectF(x1, y0, v.dx + v.dw, y1), g.brush.Get());
        g.brush->SetColor(UiColor(0xFFFFFF, 0.95f));
        g.d2d->DrawRectangle(D2D1::RectF(x0, y0, x1, y1), g.brush.Get(), 1.5f * S);
        if (g.tool == T_CROP_BOX) {
            const float hs = 5*S;
            for (P hp : { P{x0,y0}, P{(x0+x1)*0.5f,y0}, P{x1,y0}, P{x1,(y0+y1)*0.5f},
                          P{x1,y1}, P{(x0+x1)*0.5f,y1}, P{x0,y1}, P{x0,(y0+y1)*0.5f} }) {
                g.brush->SetColor(UiColor(0x17191D, 1.f));
                g.d2d->FillEllipse(D2D1::Ellipse(hp, hs, hs), g.brush.Get());
                g.brush->SetColor(UiColor(0xFFFFFF, 0.95f));
                g.d2d->DrawEllipse(D2D1::Ellipse(hp, hs, hs), g.brush.Get(), 1.2f*S);
            }
            // Botón de confirmación: el recorte no se aplica hasta pulsar este ✓.
            {
                float bs = 28*S;
                float bx = x1 - bs - 8*S, by = y1 - bs - 8*S;
                D2D1_ROUNDED_RECT br = D2D1::RoundedRect(D2D1::RectF(bx,by,bx+bs,by+bs),7*S,7*S);
                g.brush->SetColor(UiColor(0x17191D,0.96f));
                g.d2d->FillRoundedRectangle(br,g.brush.Get());
                g.brush->SetColor(UiColor(0xFFFFFF,0.96f));
                g.d2d->DrawRoundedRectangle(br,g.brush.Get(),1.2f*S);
                g.d2d->DrawLine(P{bx+7*S,by+14*S},P{bx+12*S,by+19*S},g.brush.Get(),2.2f*S,g.ssRound.Get());
                g.d2d->DrawLine(P{bx+12*S,by+19*S},P{bx+21*S,by+9*S},g.brush.Get(),2.2f*S,g.ssRound.Get());
            }
        }
    }

    // flechas laterales (solo sin herramienta activa)
    if (g.tool == T_NONE && g.files.size() > 1 && g.hover != 0) {
        float cy = top + (H - top) / 2;
        if (g.hover == 1) DrawChevron(26 * S, cy, -1, S);
        else DrawChevron(W - 26 * S, cy, 1, S);
    }

    // panel de informacion
    if (g.showInfo && g.srv) {
        auto rows = InfoRows();
        float pw = 360 * S, rh = 24 * S, px = W - pw - 10 * S, py = top + 8 * S;
        float ph = rh * rows.size() + 16 * S;
        D2D1_ROUNDED_RECT pr = D2D1::RoundedRect(D2D1::RectF(px, py, px + pw, py + ph), 10 * S, 10 * S);
        g.brush->SetColor(UiColor(0x1C1F23, 0.93f)); g.d2d->FillRoundedRectangle(pr, g.brush.Get());
        g.brush->SetColor(UiColor(0x3A4046, 1.f)); g.d2d->DrawRoundedRectangle(pr, g.brush.Get(), 1.f * S);
        float y = py + 8 * S;
        for (auto& kv : rows) {
            D2D1_RECT_F lr{ px + 14 * S, y, px + 142 * S, y + rh }, vr{ px + 146 * S, y, px + pw - 10 * S, y + rh };
            if (g.fLeft) {
                g.brush->SetColor(UiColor(0x9AA0A6));
                g.d2d->DrawText(kv.first.c_str(), (UINT32)kv.first.size(), g.fLeft.Get(), lr, g.brush.Get());
                g.brush->SetColor(UiColor(0xF2F2F2));
                g.d2d->PushAxisAlignedClip(vr, D2D1_ANTIALIAS_MODE_ALIASED);
                g.d2d->DrawText(kv.second.c_str(), (UINT32)kv.second.size(), g.fLeft.Get(), vr, g.brush.Get());
                g.d2d->PopAxisAlignedClip();
            }
            y += rh;
        }
    }

    // barra superior: aparece al llevar el ratón a la zona superior.
    if (g.showTop) {
        Layout(W, S);
        // Fondo oscuro muy sutil para mantener los iconos legibles sobre imágenes claras.
        // La barra sigue siendo transparente; solo se añade una ligera capa negra.
        g.brush->SetColor(UiColor(0x000000, 0.43f));
        g.d2d->FillRectangle(D2D1::RectF(0, 0, W, top), g.brush.Get());
        for (float sx : g.seps) {
            g.brush->SetColor(UiColor(0x3A4046, 1.f));
            g.d2d->DrawLine(P{ sx, 12 * S }, P{ sx, top - 12 * S }, g.brush.Get(), 1.f);
        }
        for (auto& b : g.btns) DrawButton(b, S);
    }

    DrawAdjustPanel(W);

    // Botón de pantalla completa, fijo abajo a la derecha.
    // En pantalla completa permanece oculto hasta que el ratón entra en su zona.
    if (g.tool == T_NONE && (!g.full || g.hoverBtn == 1001)) {
        const float bw = 44 * S, bh = 40 * S, m = 10 * S;
        D2D1_RECT_F br = D2D1::RectF(W - bw - m, H - bh - m, W - m, H - m);
        D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(br, 7 * S, 7 * S);
        bool hov = g.hoverBtn == 1001;
        if (hov) {
            g.brush->SetColor(UiColor(0xFFFFFF, 0.10f));
            g.d2d->FillRoundedRectangle(rr, g.brush.Get());
        }
        g.brush->SetColor(UiColor(0xF2F2F2, 0.95f));
        float cx = (br.left + br.right) * 0.5f, cy = (br.top + br.bottom) * 0.5f;
        float q = 9 * S, z = 4 * S;
        for (float sx : { -1.f, 1.f }) for (float sy : { -1.f, 1.f }) {
            g.d2d->DrawLine(P{cx+sx*q,cy+sy*q}, P{cx+sx*q,cy+sy*z}, g.brush.Get(), 1.8f*S);
            g.d2d->DrawLine(P{cx+sx*q,cy+sy*q}, P{cx+sx*z,cy+sy*q}, g.brush.Get(), 1.8f*S);
        }
    }

    if (g.dialog) {
        g.brush->SetColor(UiColor(0x000000, 0.42f));
        g.d2d->FillRectangle(D2D1::RectF(0, 0, W, H), g.brush.Get());
        const DialogGeo D = GetDialogGeo(W, H, S);
        auto rr = D2D1::RoundedRect(D2D1::RectF(D.px, D.py, D.px + D.pw, D.py + D.ph), 10 * S, 10 * S);
        g.brush->SetColor(UiColor(0x24272B, 0.98f)); g.d2d->FillRoundedRectangle(rr, g.brush.Get());
        g.brush->SetColor(UiColor(0x4A5058, 0.95f)); g.d2d->DrawRoundedRectangle(rr, g.brush.Get(), 1 * S);
        g.brush->SetColor(UiColor(0xF2F2F2));
        const wchar_t* title = g.dialog == 1 ? T(L"Guardar imagen") : T(L"Cambios sin guardar");
        const wchar_t* msg = g.dialog == 1 ? T(L"Se sobrescribirá el archivo original. ¿Continuar?")
                                         : T(L"Hay cambios sin guardar. ¿Qué quieres hacer?");
        g.d2d->DrawText(title, (UINT32)wcslen(title), g.fCenter.Get(), D2D1::RectF(D.px + 18 * S, D.py + 18 * S, D.px + D.pw - 18 * S, D.py + 46 * S), g.brush.Get());
        g.brush->SetColor(UiColor(0xC8CCD1));
        g.d2d->DrawText(msg, (UINT32)wcslen(msg), g.fCenter.Get(), D2D1::RectF(D.px + 24 * S, D.py + 52 * S, D.px + D.pw - 24 * S, D.py + 92 * S), g.brush.Get());
        static const wchar_t* labels1[] = { T(L"Sí"), T(L"No") };
        static const wchar_t* labels2[] = { T(L"Guardar"), T(L"No guardar"), T(L"Cancelar") };
        static const uint32_t fills[] = { 0x2F80ED, 0x555B62, 0x3B4148 };
        const wchar_t** labels = g.dialog == 1 ? labels1 : labels2;
        for (int i = 0; i < D.n; i++) {
            g.brush->SetColor(UiColor(fills[i]));
            g.d2d->FillRoundedRectangle(D2D1::RoundedRect(D.b[i], 7 * S, 7 * S), g.brush.Get());
        }
        g.brush->SetColor(UiColor(0xFFFFFF));
        for (int i = 0; i < D.n; i++)
            g.d2d->DrawText(labels[i], (UINT32)wcslen(labels[i]), g.fCenter.Get(), D.b[i], g.brush.Get());
    }

    if (g.defaultPrompt) {
        g.brush->SetColor(UiColor(0x000000, 0.48f));
        g.d2d->FillRectangle(D2D1::RectF(0, 0, W, H), g.brush.Get());
        const PromptGeo D = GetPromptGeo(W, H, S);
        auto rr = D2D1::RoundedRect(D2D1::RectF(D.px, D.py, D.px + D.pw, D.py + D.ph), 10 * S, 10 * S);
        g.brush->SetColor(UiColor(0x24272B, 0.98f)); g.d2d->FillRoundedRectangle(rr, g.brush.Get());
        g.brush->SetColor(UiColor(0x4A5058, 0.95f)); g.d2d->DrawRoundedRectangle(rr, g.brush.Get(), 1 * S);
        g.brush->SetColor(UiColor(0xF2F2F2));
        g.d2d->DrawText(T(L"¿Quieres usar PikViewer como visor predeterminado?"), (UINT32)wcslen(T(L"¿Quieres usar PikViewer como visor predeterminado?")), g.fCenter.Get(), D2D1::RectF(D.px + 20 * S, D.py + 24 * S, D.px + D.pw - 20 * S, D.py + 58 * S), g.brush.Get());
        g.brush->SetColor(UiColor(0xC8CCD1));
        const wchar_t* msg = T(L"Se abrirá la configuración de Windows para que puedas establecer PikViewer como\npredeterminado para sus formatos de imagen compatibles.");
        g.d2d->DrawText(msg, (UINT32)wcslen(msg), g.fWrap.Get(), D2D1::RectF(D.px + 28 * S, D.py + 82 * S, D.px + D.pw - 28 * S, D.py + 145 * S), g.brush.Get());
        g.brush->SetColor(UiColor(0x2F80ED)); g.d2d->FillRoundedRectangle(D2D1::RoundedRect(D.yes, 7 * S, 7 * S), g.brush.Get());
        g.brush->SetColor(UiColor(0x555B62)); g.d2d->FillRoundedRectangle(D2D1::RoundedRect(D.no, 7 * S, 7 * S), g.brush.Get());
        g.brush->SetColor(UiColor(0xFFFFFF));
        g.d2d->DrawText(T(L"Sí"), (UINT32)wcslen(T(L"Sí")), g.fCenter.Get(), D.yes, g.brush.Get());
        g.d2d->DrawText(T(L"No"), (UINT32)wcslen(T(L"No")), g.fCenter.Get(), D.no, g.brush.Get());
    }
    g.d2d->EndDraw();
}

static void Render() {
    if (!g.sc || !g.rtv) return;
    RECT rc; GetClientRect(g.hwnd, &rc);
    float W = (float)std::max<LONG>(1, rc.right), H = (float)std::max<LONG>(1, rc.bottom);
    const float black[4] = { 0, 0, 0, 1 };
    g.ctx->OMSetRenderTargets(1, g.rtv.GetAddressOf(), nullptr);
    g.ctx->ClearRenderTargetView(g.rtv.Get(), black);
    D3D11_VIEWPORT vp{ 0, 0, W, H, 0, 1 };
    g.ctx->RSSetViewports(1, &vp);
    SetupPipeline();
    if (g.srv) {
        View v = GetView();
        Cbuf cb{};
        cb.scale[0] = v.dw / W; cb.scale[1] = v.dh / H;
        cb.offset[0] = (v.dx + v.dw * 0.5f) / W * 2 - 1;
        cb.offset[1] = 1 - (v.dy + v.dh * 0.5f) / H * 2;
        cb.sdrWhite = g.white; cb.isHdr = g.hdr ? 1.f : 0.f; cb.eotf = (float)g.eotf;
        cb.gamma = g.gamma; cb.brightness = g.brightness; cb.contrast = g.contrast; cb.saturation = g.saturation;
        cb.uvMin[0] = v.c.x / g.imgW; cb.uvMin[1] = v.c.y / g.imgH;
        cb.uvMax[0] = (v.c.x + v.c.w) / g.imgW; cb.uvMax[1] = (v.c.y + v.c.h) / g.imgH;
        g.ctx->UpdateSubresource(g.cb.Get(), 0, nullptr, &cb, 0, 0);
        g.ctx->PSSetShaderResources(0, 1, g.srv.GetAddressOf());
        g.ctx->Draw(4, 0);
    }
    DrawUI(W, H);
    g.sc->Present(1, 0);
}

static int ShowMenu(int btnId, const std::vector<MI>& items) {
    HMENU m = CreatePopupMenu();
    MENUINFO mi{ sizeof(mi) };
    mi.fMask = MIM_BACKGROUND | MIM_APPLYTOSUBMENUS;
    mi.hbrBack = CreateSolidBrush(RGB(28,31,35));
    SetMenuInfo(m, &mi);
    for (auto& i : items) {
        if (i.id == 0) AppendMenuW(m, MF_OWNERDRAW, 0, nullptr);
        else AppendMenuW(m, MF_OWNERDRAW | (i.chk ? MF_CHECKED : 0), i.id, (LPCWSTR)i.t);
    }
    POINT pt{ 0, 0 };
    for (auto& b : g.btns) if (b.id == btnId) { pt.x = (LONG)b.r.left; pt.y = (LONG)b.r.bottom; }
    ClientToScreen(g.hwnd, &pt);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, 0, g.hwnd, nullptr);
    DeleteObject(mi.hbrBack);
    DestroyMenu(m);
    return cmd;
}

static void DrawPopupMenuItem(const DRAWITEMSTRUCT* dis) {
    float S = Dpi();
    HDC dc = dis->hDC;
    RECT r = dis->rcItem;
    bool selected = (dis->itemState & ODS_SELECTED) != 0;
    bool checked = (dis->itemState & ODS_CHECKED) != 0;
    bool disabled = (dis->itemState & ODS_DISABLED) != 0;
    if (!dis->itemData) {
        HBRUSH bg = CreateSolidBrush(RGB(28,31,35));
        FillRect(dc, &r, bg); DeleteObject(bg);
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(58,64,70));
        HGDIOBJ old = SelectObject(dc, pen);
        MoveToEx(dc, r.left + (LONG)(12*S), (r.top+r.bottom)/2, nullptr);
        LineTo(dc, r.right - (LONG)(12*S), (r.top+r.bottom)/2);
        SelectObject(dc, old); DeleteObject(pen);
        return;
    }
    HBRUSH bg = CreateSolidBrush(selected ? RGB(53,58,64) : RGB(28,31,35));
    FillRect(dc, &r, bg);
    DeleteObject(bg);
    if (checked) {
        HBRUSH hb = CreateSolidBrush(RGB(76,194,255));
        RECT cr{ r.left + (LONG)(12*S), r.top + (LONG)(10*S), r.left + (LONG)(18*S), r.bottom - (LONG)(10*S) };
        FillRect(dc, &cr, hb); DeleteObject(hb);
    }
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, disabled ? RGB(95,100,106) : RGB(242,242,242));
    RECT tr = r;
    tr.left += (LONG)(22*S); tr.right -= (LONG)(14*S);
    DrawTextW(dc, (LPCWSTR)dis->itemData, -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
}

static void MeasurePopupMenuItem(MEASUREITEMSTRUCT* mi) {
    float S = Dpi();
    mi->itemWidth = (UINT)(230*S);
    mi->itemHeight = (UINT)(34*S);
}

static void SetTool(int t) {
    bool turningOff = (g.tool == t);
    g.tool = turningOff ? T_NONE : t;
    g.hover = 0;
    g.resizeField = 0; g.resizeSelectAll = false;
    KillCaretTimer();
    g.cropHandle = 0;
    g.cropBoxChanged = false;
    if (g.tool == T_CROP_BOX && g.srv) {
        g.cropping = true;
        CR c = CropRect();
        g.cropA = P{c.x, c.y};
        g.cropB = P{c.x + c.w, c.y + c.h};
    } else if (g.tool != T_CROP && g.tool != T_CROP_BOX) {
        g.cropping = false;
    }
    if (g.tool != T_NONE) {
        g.showTop = false;
        g.showInfo = false;
        g.showAdjust = false;
        g.hoverBtn = 0;
        KillTimer(g.hwnd, kToolbarTimer);
    }
    Render();
}

static void ToggleFullscreen() {
    DWORD st = GetWindowLongW(g.hwnd, GWL_STYLE);
    if (!g.full) {
        GetWindowPlacement(g.hwnd, &g.prev);
        MONITORINFO mi{ sizeof(mi) };
        GetMonitorInfoW(MonitorFromWindow(g.hwnd, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLongW(g.hwnd, GWL_STYLE, st & ~WS_OVERLAPPEDWINDOW);
        SetWindowLongW(g.hwnd, GWL_EXSTYLE, GetWindowLongW(g.hwnd, GWL_EXSTYLE) & ~WS_EX_DLGMODALFRAME);
        SetWindowPos(g.hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
    } else {
        SetWindowLongW(g.hwnd, GWL_STYLE, st | WS_OVERLAPPEDWINDOW);
        SetWindowLongW(g.hwnd, GWL_EXSTYLE, GetWindowLongW(g.hwnd, GWL_EXSTYLE) | WS_EX_DLGMODALFRAME);
        SetWindowPlacement(g.hwnd, &g.prev);
        SetWindowPos(g.hwnd, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
    g.full = !g.full;
    // Al entrar en pantalla completa ocultamos el botón inmediatamente.
    // Volverá a mostrarse cuando el ratón entre en su zona.
    if (g.full) g.hoverBtn = 0;
}

static void SyncResizeFieldsFromCrop();
static void ApplyResize();

static void OnButton(int id) {
    switch (id) {
    case B_FILE: {
        int c = ShowMenu(B_FILE, { { 101, T(L"Abrir…"), false }, { 102, T(L"Guardar"), false },
                                   { 103, T(L"Guardar como…"), false }, { 0, nullptr, false },
                                   { 104, T(L"Salir"), false } });
        if (c == 101) DoOpen(); else if (c == 102) DoSave(); else if (c == 103) DoSaveAs();
        else if (c == 104) PostMessageW(g.hwnd, WM_CLOSE, 0, 0);
        break;
    }
    case B_PEN: SetTool(T_PEN); break;
    case B_HIGH: SetTool(T_HIGH); break;
    case B_ERASE: SetTool(T_ERASE); break;
    case B_CROP: SetTool(T_CROP); break;
    case B_CROP_BOX: SetTool(T_CROP_BOX); break;
    case B_SHAPE: {
        int c = ShowMenu(B_SHAPE, { { 201, T(L"Rectángulo"), g.tool == T_RECT }, { 202, T(L"Elipse"), g.tool == T_ELL },
                                    { 203, T(L"Línea"), g.tool == T_LINE }, { 204, T(L"Flecha"), g.tool == T_ARROW } });
        if (c >= 201 && c <= 204) g.tool = T_RECT + (c - 201);
        break;
    }
    case B_COLOR: {
        std::vector<MI> items;
        for (int i = 0; i < (int)_countof(kColors); i++) items.push_back({ 300 + i, T(kColors[i].name), kColors[i].rgb == g.color });
        int c = ShowMenu(B_COLOR, items);
        if (c >= 300 && c < 300 + (int)_countof(kColors)) g.color = kColors[c - 300].rgb;
        break;
    }
    case B_WIDTH: {
        int c = ShowMenu(B_WIDTH, { { 400, T(L"Fino"), g.widthIdx == 0 }, { 401, T(L"Medio"), g.widthIdx == 1 },
                                    { 402, T(L"Grueso"), g.widthIdx == 2 }, { 403, T(L"Muy grueso"), g.widthIdx == 3 } });
        if (c >= 400 && c <= 403) g.widthIdx = c - 400;
        break;
    }
    case B_UNDO: DoUndo(); return;
    case B_REDO: DoRedo(); return;
    case B_INFO:
        g.showInfo = !g.showInfo;
        if (g.showInfo) g.showAdjust = false;
        break;
    case B_ADJUST:
        g.showAdjust = !g.showAdjust;
        g.resizeField = 0; g.resizeSelectAll = false;
        KillCaretTimer();
        if (g.showAdjust) { g.showInfo = false; SyncResizeFieldsFromCrop(); }
        break;
    case B_SAVE: DoSave(); break;
    }
    Render();
}

static int HitBtn(int x, int y) {
    for (auto& b : g.btns)
        if (In(b.r, x, y)) return b.id;
    return 0;
}

static int Zone(int x, int y) {
    if (g.tool != T_NONE || g.files.size() < 2 || y < TopH()) return 0;
    RECT rc; GetClientRect(g.hwnd, &rc);
    int z = (int)(90 * Dpi());
    return x < z ? 1 : (x > rc.right - z ? 2 : 0);
}

// Zoom centrado en el punto bajo el cursor.
static void ZoomAt(int x, int y, float factor) {
    if (!g.srv) return;
    View before = GetView();
    if (x < before.dx || x > before.dx + before.dw || y < before.dy || y > before.dy + before.dh) return;

    P anchor = ToImg(before, x, y);
    g.zoom = std::clamp(g.zoom * factor, 1.0f, 20.0f);
    if (g.zoom <= 1.0001f) {
        g.zoom = 1.0f;
        g.panX = 0.0f;
        g.panY = 0.0f;
        Render();
        return;
    }

    RECT rc; GetClientRect(g.hwnd, &rc);
    float W = (float)std::max<LONG>(1, rc.right);
    float H = (float)std::max<LONG>(1, rc.bottom);
    float top = UiTop();
    float ah = std::max(1.0f, H - top);
    View after = GetView();

    g.panX = anchor.x - after.c.x - after.c.w * 0.5f
           - (x - W * 0.5f) / after.f;
    g.panY = anchor.y - after.c.y - after.c.h * 0.5f
           - (y - (top + ah * 0.5f)) / after.f;
    Render();
}

static bool HitFullscreen(int x, int y) {
    if (g.tool != T_NONE) return false;
    float S = Dpi();
    RECT rc; GetClientRect(g.hwnd, &rc);
    float bw = 44 * S, bh = 40 * S, m = 10 * S;
    return x >= rc.right - bw - m && x <= rc.right - m &&
           y >= rc.bottom - bh - m && y <= rc.bottom - m;
}

static int RunConfirmDialog(int kind) {
    g.dialog = kind;
    g.dialogResult = 0;
    Render();
    MSG msg{};
    while (g.dialog && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return g.dialogResult;
}

static bool HandleDialogClick(int x, int y) {
    if (!g.dialog) return false;
    RECT rc{}; GetClientRect(g.hwnd, &rc);
    const DialogGeo D = GetDialogGeo((float)rc.right, (float)rc.bottom, Dpi());
    // Resultado por botón: Sí/Guardar = 1, No guardar = 2, No/Cancelar = 3.
    static const int res1[] = { 1, 3 }, res2[] = { 1, 2, 3 };
    const int* res = g.dialog == 1 ? res1 : res2;
    for (int i = 0; i < D.n; i++)
        if (In(D.b[i], x, y)) { g.dialogResult = res[i]; g.dialog = 0; Render(); return true; }
    return true;
}

static bool HandleDefaultPromptClick(int x, int y) {
    if (!g.defaultPrompt) return false;
    RECT rc; GetClientRect(g.hwnd, &rc);
    const PromptGeo D = GetPromptGeo((float)std::max<LONG>(1, rc.right), (float)std::max<LONG>(1, rc.bottom), Dpi());
    bool yes = In(D.yes, x, y), no = In(D.no, x, y);
    if (yes || no) {
        g.defaultPrompt = false;
        MarkDefaultPromptDone();
        if (yes) OpenDefaultAppsSettings();
        Render();
    }
    return true;
}

static void RefreshAdjustDirty() {
    g.adjustDirty = fabsf(g.gamma) > 0.0001f || fabsf(g.brightness) > 0.0001f ||
                     fabsf(g.contrast) > 0.0001f || fabsf(g.saturation) > 0.0001f;
}

// Asigna un valor a un ajuste y refresca estado, título y pantalla.
static void ApplyAdjust(int i, float v) {
    AdjustValue(i) = v;
    RefreshAdjustDirty(); UpdateTitle(); Render();
}

static bool HandleAdjustClick(int x, int y, bool dbl) {
    if (!g.showAdjust) return false;
    RECT rc{}; GetClientRect(g.hwnd, &rc);
    auto p = AdjustPanelRect((float)rc.right);
    float S = Dpi();
    if (!In(p, x, y)) { g.showAdjust = false; g.resizeField = 0; g.resizeSelectAll = false; KillCaretTimer(); Render(); return true; }

    const AdjustGeo A = GetAdjustGeo(p, S);
    for (int i = 0; i < 4; i++) {
        float cy = A.start + i * A.row;
        if (In(D2D1::RectF(A.resetL, cy - 17 * S, A.x1, cy + 17 * S), x, y)) {
            if (fabsf(AdjustValue(i)) > 0.0001f) PushUndo(false);
            ApplyAdjust(i, 0.f); return true;
        }
        if (fabsf(y - cy) <= 14 * S && x >= A.sx - 10 * S && x <= A.ex + 10 * S) {
            if (dbl) {
                if (fabsf(AdjustValue(i)) > 0.0001f) PushUndo(false);
                ApplyAdjust(i, 0.f); return true;
            }
            PushUndo(false);
            g.adjustDrag = i; SetCapture(g.hwnd);
            ApplyAdjust(i, SliderValue(A, x));
            return true;
        }
    }

    if (In(ResizeWidthBox(p,S), x, y)) {
        if (!g.resizeDirty) SyncResizeFieldsFromCrop();
        if (g.resizeAspect <= 0) { int w=ParsePositiveInt(g.resizeWText), h=ParsePositiveInt(g.resizeHText); g.resizeAspect = h ? (float)w/h : 1.0f; }
        g.resizeField = 1; g.resizeSelectAll = true; SetFocus(g.hwnd);
        ArmCaretTimer(); Render(); return true;
    }
    if (In(ResizeHeightBox(p,S), x, y)) {
        if (!g.resizeDirty) SyncResizeFieldsFromCrop();
        if (g.resizeAspect <= 0) { int w=ParsePositiveInt(g.resizeWText), h=ParsePositiveInt(g.resizeHText); g.resizeAspect = h ? (float)w/h : 1.0f; }
        g.resizeField = 2; g.resizeSelectAll = true; SetFocus(g.hwnd);
        ArmCaretTimer(); Render(); return true;
    }
    if (In(ResizeKeepBox(p,S), x, y)) {
        g.keepAspect = !g.keepAspect;
        if (g.keepAspect) { int w=ParsePositiveInt(g.resizeWText), h=ParsePositiveInt(g.resizeHText); if (h) g.resizeAspect=(float)w/h; }
        Render(); return true;
    }
    if (In(ResizeApplyBox(p,S), x, y)) { ApplyResize(); return true; }
    return true;
}

static void UpdateAdjustDrag(int x) {
    if (g.adjustDrag < 0) return;
    RECT rc{}; GetClientRect(g.hwnd, &rc);
    const AdjustGeo A = GetAdjustGeo(AdjustPanelRect((float)rc.right), Dpi());
    ApplyAdjust(g.adjustDrag, SliderValue(A, x));
}

static int HitCropHandle(int x, int y) {
    if (g.tool != T_CROP_BOX || !g.cropping || !g.srv) return 0;
    View v = GetView();
    float x0 = v.dx + (g.cropA.x - v.c.x) * v.f, x1 = v.dx + (g.cropB.x - v.c.x) * v.f;
    float y0 = v.dy + (g.cropA.y - v.c.y) * v.f, y1 = v.dy + (g.cropB.y - v.c.y) * v.f;
    float tol = 24.0f * Dpi();
    bool L = fabsf(x-x0)<=tol, R = fabsf(x-x1)<=tol, T = fabsf(y-y0)<=tol, B = fabsf(y-y1)<=tol;
    bool inX = x >= x0-tol && x <= x1+tol, inY = y >= y0-tol && y <= y1+tol;
    if (L && T) return 1; if (T && R) return 2; if (R && B) return 3; if (B && L) return 4;
    if (T && inX) return 5; if (R && inY) return 6; if (B && inX) return 7; if (L && inY) return 8;
    return 0;
}

static void UpdateCropBox(P p) {
    p.x = std::clamp(p.x, 0.0f, (float)g.imgW);
    p.y = std::clamp(p.y, 0.0f, (float)g.imgH);
    float minW = 8.0f, minH = 8.0f;
    switch (g.cropHandle) {
    case 1: g.cropA.x = std::min(p.x, g.cropB.x-minW); g.cropA.y = std::min(p.y, g.cropB.y-minH); break;
    case 2: g.cropB.x = std::max(p.x, g.cropA.x+minW); g.cropA.y = std::min(p.y, g.cropB.y-minH); break;
    case 3: g.cropB.x = std::max(p.x, g.cropA.x+minW); g.cropB.y = std::max(p.y, g.cropA.y+minH); break;
    case 4: g.cropA.x = std::min(p.x, g.cropB.x-minW); g.cropB.y = std::max(p.y, g.cropA.y+minH); break;
    case 5: g.cropA.y = std::min(p.y, g.cropB.y-minH); break;
    case 6: g.cropB.x = std::max(p.x, g.cropA.x+minW); break;
    case 7: g.cropB.y = std::max(p.y, g.cropA.y+minH); break;
    case 8: g.cropA.x = std::min(p.x, g.cropB.x-minW); break;
    }
    g.cropBoxChanged = true;
}

static bool HitCropConfirm(int x, int y) {
    if (g.tool != T_CROP_BOX || !g.cropping || !g.srv) return false;
    View v = GetView();
    float x0 = v.dx + (g.cropA.x - v.c.x) * v.f, x1 = v.dx + (g.cropB.x - v.c.x) * v.f;
    float y0 = v.dy + (g.cropA.y - v.c.y) * v.f, y1 = v.dy + (g.cropB.y - v.c.y) * v.f;
    float bs = 28*Dpi(), bx = x1-bs-8*Dpi(), by = y1-bs-8*Dpi();
    return x >= bx && x <= bx+bs && y >= by && y <= by+bs;
}

// Operación de recorte a partir de las esquinas A/B actuales (redondeadas hacia fuera).
static Op MakeCropOp() {
    Op o; o.type = T_CROP;
    o.pts = { P{ floorf(std::min(g.cropA.x, g.cropB.x)), floorf(std::min(g.cropA.y, g.cropB.y)) },
              P{ ceilf(std::max(g.cropA.x, g.cropB.x)), ceilf(std::max(g.cropA.y, g.cropB.y)) } };
    return o;
}

static void ConfirmCropBox() {
    if (g.tool != T_CROP_BOX || !g.cropping || !g.srv) return;
    float w = fabsf(g.cropB.x - g.cropA.x), h = fabsf(g.cropB.y - g.cropA.y);
    if (w <= 8.f || h <= 8.f) return;
    PushUndo(); g.ops.push_back(MakeCropOp());
    g.panX = 0.0f; g.panY = 0.0f;
    UpdateTitle();
    CR c = CropRect();
    g.cropA = P{c.x, c.y}; g.cropB = P{c.x + c.w, c.y + c.h};
    g.cropBoxChanged = false;
    Render();
}

static void OnLDown(int x, int y, bool dbl) {
    if (HandleDialogClick(x, y)) return;
    if (HandleDefaultPromptClick(x, y)) return;
    if (g.showAdjust && y >= TopH()) { if (HandleAdjustClick(x, y, dbl)) return; }
    if (HitFullscreen(x, y)) { ToggleFullscreen(); Render(); return; }
    if (g.showTop && y < TopH()) { int id = HitBtn(x, y); if (id) OnButton(id); return; }
    if (g.tool == T_NONE) {
        int z = Zone(x, y);
        if (z == 1) Navigate(-1); else if (z == 2) Navigate(1); else if (dbl) ToggleFullscreen();
        return;
    }
    if (!g.canEdit || !g.srv) return;
    View v = GetView();
    if (x < v.dx || x > v.dx + v.dw || y < v.dy || y > v.dy + v.dh) return;
    P p = ToImg(v, x, y);
    SetCapture(g.hwnd);
    g.mouseDown = true; g.erased = false;
    float w = kWidths[g.widthIdx] * Dpi() / v.f;
    switch (g.tool) {
    case T_CROP_BOX: {
        if (HitCropConfirm(x, y)) {
            ReleaseCapture(); g.mouseDown = false;
            ConfirmCropBox();
            return;
        }
        int h = HitCropHandle(x, y);
        if (!h) return;
        g.cropHandle = h;
        g.cropBoxChanged = false;
        g.mouseDown = true;
        return;
    }
    case T_PEN: case T_HIGH:
        g.cur = Op(); g.cur.type = g.tool; g.cur.color = g.color; g.cur.pts = { p };
        g.cur.width = g.tool == T_HIGH ? w * 3.f : w;
        g.drawing = true; Redraw(&g.cur); Render(); break;
    case T_RECT: case T_ELL: case T_LINE: case T_ARROW:
        g.cur = Op(); g.cur.type = g.tool; g.cur.color = g.color; g.cur.width = w; g.cur.pts = { p, p };
        g.drawing = true; break;
    case T_CROP: g.cropping = true; g.cropA = g.cropB = p; Render(); break;
    case T_ERASE: EraseAt(p, v.f); break;
    }
}

static void OnMouseMove(int x, int y) {
    g.mx = x; g.my = y;
    UpdateToolbarHover(y);
    if (g.adjustDrag >= 0) { UpdateAdjustDrag(x); return; }
    if (g.panning) {
        float f = GetView().f;
        g.panX = g.panOrigX - (x - g.panStartX) / std::max(0.0001f, f);
        g.panY = g.panOrigY - (y - g.panStartY) / std::max(0.0001f, f);
        Render();
        return;
    }
    if (g.mouseDown && g.srv) {
        View v = GetView();
        P p = ToImg(v, x, y);
        if (g.drawing) {
            if (g.cur.type == T_PEN || g.cur.type == T_HIGH) {
                P l = g.cur.pts.back();
                if (fabsf(p.x - l.x) + fabsf(p.y - l.y) < 0.5f / v.f) return;
                g.cur.pts.push_back(p);
            } else g.cur.pts[1] = p;
            Redraw(&g.cur); Render();
        } else if (g.tool == T_CROP_BOX && g.cropHandle) { UpdateCropBox(p); Render(); }
        else if (g.cropping && g.tool == T_CROP) { g.cropB = p; Render(); }
        else if (g.tool == T_ERASE) EraseAt(p, v.f);
        return;
    }
    int hb = (g.showTop && y < TopH()) ? HitBtn(x, y) : (HitFullscreen(x, y) ? 1001 : 0);
    int hz = Zone(x, y);
    if (hb != g.hoverBtn || hz != g.hover) { g.hoverBtn = hb; g.hover = hz; Render(); }
}

static void OnLUp() {
    if (g.adjustDrag >= 0) { g.adjustDrag = -1; ReleaseCapture(); return; }
    if (!g.mouseDown) return;
    ReleaseCapture();
    g.mouseDown = false;
    if (g.drawing) {
        g.drawing = false;
        View v = GetView();
        bool tiny = false;
        if (g.cur.type >= T_RECT && g.cur.type <= T_ARROW) {
            float dx = g.cur.pts[1].x - g.cur.pts[0].x, dy = g.cur.pts[1].y - g.cur.pts[0].y;
            tiny = sqrtf(dx * dx + dy * dy) * v.f < 3.f;
        }
        if (!tiny) { PushUndo(); g.ops.push_back(g.cur); }
        Redraw(nullptr); UpdateTitle(); Render();
    } else if (g.tool == T_CROP_BOX && g.cropHandle) {
        // Soltar un tirador solo mueve/redimensiona el marco.
        // El recorte se aplica exclusivamente al pulsar el botón ✓.
        g.cropHandle = 0;
        g.cropBoxChanged = false;
        Render();
    } else if (g.cropping && g.tool == T_CROP) {
        g.cropping = false;
        View v = GetView();
        float w = fabsf(g.cropB.x - g.cropA.x), h = fabsf(g.cropB.y - g.cropA.y);
        if (w * v.f > 8.f && h * v.f > 8.f) {
            PushUndo(); g.ops.push_back(MakeCropOp());
            g.panX = 0.0f; g.panY = 0.0f;
            UpdateTitle();
        }
        Render();
    }
}

// ------------------------------------------------------------------ ventana
static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_SIZE:
        if (w != SIZE_MINIMIZED) { Resize(); Render(); }
        return 0;
    case WM_PAINT: { PAINTSTRUCT ps; BeginPaint(h, &ps); EndPaint(h, &ps); Render(); return 0; }
    case WM_ERASEBKGND: return 1;
    case WM_ACTIVATE: case WM_DISPLAYCHANGE:
        UpdateWhite(); Render(); return 0;
    case WM_DPICHANGED: {
        RECT* r = (RECT*)l;
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER);
        MakeFonts();
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(l) == HTCLIENT && g.tool != T_NONE && g.my > UiTop()) { SetCursor(LoadCursorW(nullptr, IDC_CROSS)); return TRUE; }
        break;
    case WM_TIMER:
        if (w == kToolbarTimer) {
            POINT pt{}; GetCursorPos(&pt); ScreenToClient(h, &pt);
            if (g.tool != T_NONE) { g.showTop = false; g.hoverBtn = 0; KillTimer(h, kToolbarTimer); Render(); return 0; }
            if (g.showTop && pt.y >= TopH() && !PointInAdjustPanel(pt.x, pt.y) && !g.mouseDown) {
                g.showTop = false; g.showAdjust = false; g.hoverBtn = 0; g.resizeField = 0; KillCaretTimer();
                KillTimer(h, kToolbarTimer); Render();
            }
            return 0;
        }
        if (w == kGifTimer) { GifTick(); return 0; }
        if (w == kCaretTimer) {
            if (g.resizeField != 0 && g.showAdjust) Render();
            else KillCaretTimer();
            return 0;
        }
        break;
    case WM_MOUSEMOVE: {
        TRACKMOUSEEVENT t{ sizeof(t), TME_LEAVE, h, 0 }; TrackMouseEvent(&t);
        OnMouseMove(GET_X_LPARAM(l), GET_Y_LPARAM(l));
        return 0;
    }
    case WM_MOUSELEAVE:
        if (g.hover || g.hoverBtn) { g.hover = 0; g.hoverBtn = 0; Render(); }
        return 0;
    case WM_MEASUREITEM:
        if (w == 0 && l) {
            MeasurePopupMenuItem((MEASUREITEMSTRUCT*)l);
            return TRUE;
        }
        break;
    case WM_DRAWITEM:
        if (w == 0 && l) {
            const DRAWITEMSTRUCT* dis = (const DRAWITEMSTRUCT*)l;
            if (dis->CtlType == ODT_MENU) { DrawPopupMenuItem(dis); return TRUE; }
        }
        break;
    case WM_LBUTTONDOWN: OnLDown(GET_X_LPARAM(l), GET_Y_LPARAM(l), false); return 0;
    case WM_LBUTTONDBLCLK: OnLDown(GET_X_LPARAM(l), GET_Y_LPARAM(l), true); return 0;
    case WM_LBUTTONUP: OnLUp(); return 0;
    case WM_RBUTTONDOWN:
        if (g.tool != T_NONE || g.cropping || g.drawing || g.panning) {
            g.tool = T_NONE;
            g.cropping = false;
            g.cropHandle = 0;
            g.cropBoxChanged = false;
            g.drawing = false;
            g.mouseDown = false;
            g.panning = false;
            g.showTop = false;
            g.hover = 0;
            g.hoverBtn = 0;
            g.adjustDrag = -1;
            ReleaseCapture();
            KillTimer(h, kToolbarTimer);
            Render();
            return 0;
        }
        break;
    case WM_XBUTTONDOWN:
        // Botones laterales del ratón: botón trasero = Deshacer, botón delantero = Rehacer.
        if (HIWORD(w) == XBUTTON1) { DoUndo(); Render(); }
        else if (HIWORD(w) == XBUTTON2) { DoRedo(); Render(); }
        return TRUE;
    case WM_XBUTTONUP:
        return TRUE;
    case WM_MBUTTONDOWN:
        if (g.srv) {
            g.panning = true;
            g.panStartX = GET_X_LPARAM(l); g.panStartY = GET_Y_LPARAM(l);
            g.panOrigX = g.panX; g.panOrigY = g.panY;
            SetCapture(h);
        }
        return 0;
    case WM_MBUTTONUP:
        if (g.panning) { g.panning = false; ReleaseCapture(); }
        return 0;
    case WM_MOUSEWHEEL:
        {
            int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
            // El mensaje usa coordenadas de pantalla.
            POINT pt{ x, y }; ScreenToClient(h, &pt);
            ZoomAt(pt.x, pt.y, GET_WHEEL_DELTA_WPARAM(w) > 0 ? 1.15f : (1.0f / 1.15f));
        }
        return 0;
    case WM_CHAR: {
        if (g.resizeField == 0) break;
        if (w >= L'0' && w <= L'9') {
            std::wstring& t = (g.resizeField == 1) ? g.resizeWText : g.resizeHText;
            if (g.resizeSelectAll) { t.clear(); g.resizeSelectAll = false; }
            if (t == L"0") t.clear();
            if (t.size() < 5) t.push_back((wchar_t)w);
            UpdateResizeAspectFromField(); Render();
            return 0;
        }
        if (w == VK_BACK) {
            std::wstring& t = (g.resizeField == 1) ? g.resizeWText : g.resizeHText;
            if (g.resizeSelectAll) { t.clear(); g.resizeSelectAll = false; }
            else if (!t.empty()) t.pop_back();
            UpdateResizeAspectFromField(); Render();
            return 0;
        }
        if (w == VK_RETURN) { ApplyResize(); return 0; }
        if (w == VK_ESCAPE) { g.resizeField = 0; g.resizeSelectAll = false; KillCaretTimer(); Render(); return 0; }
        return 0;
    }
    case WM_KEYDOWN: {
        if (g.dialog) { if (w == VK_ESCAPE) { g.dialogResult = 3; g.dialog = 0; Render(); } return 0; }
        if (g.defaultPrompt) return 0;
        if (g.resizeField && w == 'A' && (GetKeyState(VK_CONTROL) & 0x8000)) { g.resizeSelectAll = true; Render(); return 0; }
        bool ctrl = GetKeyState(VK_CONTROL) & 0x8000, shift = GetKeyState(VK_SHIFT) & 0x8000;
        if (ctrl) {
            if (w == 'O') DoOpen();
            else if (w == 'S') { if (shift) DoSaveAs(); else DoSave(); }
            else if (w == 'Z') { if (shift) DoRedo(); else DoUndo(); }
            else if (w == 'Y') DoRedo();
            return 0;
        }
        switch (w) {
        case VK_LEFT: case VK_UP: case VK_PRIOR: Navigate(-1); break;
        case VK_RIGHT: case VK_DOWN: case VK_NEXT: case VK_SPACE: Navigate(1); break;
        case VK_F11: case 'F': ToggleFullscreen(); break;
        case 'I': g.showInfo = !g.showInfo; Render(); break;
        case 'E': case 'G':
            g.eotf = (g.eotf + 1) % 3; SaveEotf(); UpdateTitle(); Render(); break;
        case VK_ESCAPE:
            if (g.cropping) { g.cropping = false; g.cropHandle = 0; g.cropBoxChanged = false; g.mouseDown = false; ReleaseCapture(); g.tool = T_NONE; Render(); }
            else if (g.tool != T_NONE) { g.tool = T_NONE; Render(); }
            else if (g.full) ToggleFullscreen();
            else PostMessageW(h, WM_CLOSE, 0, 0);
            break;
        }
        return 0;
    }
    case WM_DROPFILES: {
        wchar_t path[MAX_PATH];
        if (DragQueryFileW((HDROP)w, 0, path, MAX_PATH) && ConfirmDiscard()) OpenPath(path);
        DragFinish((HDROP)w);
        return 0;
    }
    case WM_CLOSE:
        if (!ConfirmDiscard()) return 0;
        DestroyWindow(h);
        return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g.wic)))) return 1;

    DWORD ev = 0, evs = sizeof(ev);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\PikViewer", L"Eotf", RRF_RT_REG_DWORD, nullptr, &ev, &evs) == ERROR_SUCCESS && ev < 3)
        g.eotf = (int)ev;

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    fs::path start;
    if (argc > 1) start = argv[1];
    LocalFree(argv);

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.style = CS_DBLCLKS; wc.lpfnWndProc = WndProc; wc.hInstance = hi;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(hi, MAKEINTRESOURCEW(IDI_PIKVIEWER_BLANK));
    wc.hIconSm = LoadIconW(hi, MAKEINTRESOURCEW(IDI_PIKVIEWER_BLANK));
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = L"PikViewerWnd";
    RegisterClassExW(&wc);

    // Tamaño inicial proporcional a la pantalla. La referencia del usuario
    // en 4K muestra una ventana de aproximadamente el 60% del ancho y del
    // alto de la pantalla. Usamos el área de trabajo para respetar la barra
    // de tareas y mantenemos la misma proporción en resoluciones menores.
    MONITORINFO mi{ sizeof(mi) };
    HMONITOR hm = MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
    GetMonitorInfoW(hm, &mi);
    int workW = mi.rcWork.right - mi.rcWork.left;
    int workH = mi.rcWork.bottom - mi.rcWork.top;
    constexpr double kInitialWidthScale = 0.60;
    constexpr double kInitialHeightScale = 0.63;
    int initialW = std::clamp((int)std::lround(workW * kInitialWidthScale), 400, std::max(400, workW - 40));
    int initialH = std::clamp((int)std::lround(workH * kInitialHeightScale), 300, std::max(300, workH - 40));

    // WS_EX_DLGMODALFRAME: Windows no dibuja ningún icono en la barra de título.
    g.hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, wc.lpszClassName, L"PikViewer", WS_OVERLAPPEDWINDOW,
                             CW_USEDEFAULT, CW_USEDEFAULT, initialW, initialH, nullptr, nullptr, hi, nullptr);
    // Permitir explícitamente que herramientas de captura de pantalla, como Recortes,
    // puedan capturar la ventana cuando PikViewer está en primer plano.
    SetWindowDisplayAffinity(g.hwnd, WDA_NONE);
    // Barra de título sin icono: icono pequeño (el de la barra de título) totalmente
    // transparente; el grande (barra de tareas / Alt+Tab) conserva el icono real.
    // No se pone icono a 0, porque entonces Windows muestra el icono genérico por defecto.
    {
        int sm = GetSystemMetricsForDpi(SM_CXSMICON, GetDpiForWindow(g.hwnd));
        int bg = GetSystemMetricsForDpi(SM_CXICON, GetDpiForWindow(g.hwnd));
        HICON hSmall = (HICON)LoadImageW(hi, MAKEINTRESOURCEW(IDI_PIKVIEWER_BLANK), IMAGE_ICON, sm, sm, LR_DEFAULTCOLOR);
        HICON hBig = (HICON)LoadImageW(hi, MAKEINTRESOURCEW(IDI_PIKVIEWER), IMAGE_ICON, bg, bg, LR_DEFAULTCOLOR);
        SendMessageW(g.hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hSmall);
        SendMessageW(g.hwnd, WM_SETICON, ICON_BIG, (LPARAM)hBig);
    }
    UpdateTitle();
    BOOL dark = TRUE;
    DwmSetWindowAttribute(g.hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
    DragAcceptFiles(g.hwnd, TRUE);
    if (!InitD3D()) { MessageBoxW(nullptr, T(L"No se pudo inicializar Direct3D 11 / Direct2D."), L"PikViewer", MB_ICONERROR); return 1; }
    ShowWindow(g.hwnd, SW_SHOW);
    UpdateWhite();
    g.defaultPrompt = ShouldShowDefaultPrompt();
    if (!start.empty()) OpenPath(start);
    else { UpdateTitle(); Render(); }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    return 0;
}
