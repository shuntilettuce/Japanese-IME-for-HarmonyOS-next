#include "display_attr.h"

#include "globals.h"

namespace shunti {
namespace win {

namespace {
TF_DISPLAYATTRIBUTE attr(TF_DA_LINESTYLE line, BOOL bold, TF_DA_ATTR_INFO info, COLORREF line_color = CLR_INVALID) {
    TF_DISPLAYATTRIBUTE a = {};
    a.crText.type = TF_CT_NONE;
    a.crBk.type = TF_CT_NONE;
    a.lsStyle = line;
    a.fBoldLine = bold;
    a.crLine.type = line_color == CLR_INVALID ? TF_CT_NONE : TF_CT_COLORREF;
    if (line_color != CLR_INVALID) a.crLine.cr = line_color;
    a.bAttr = info;
    return a;
}

struct Item { const GUID* guid; TF_DA_LINESTYLE line; BOOL bold; TF_DA_ATTR_INFO info; const wchar_t* desc; COLORREF color = CLR_INVALID; };
const Item ITEMS[] = {
    {&GUID_AttrInput, TF_LS_DOT, FALSE, TF_ATTR_INPUT, L"shunti IME 入力中"},
    {&GUID_AttrConverted, TF_LS_SOLID, FALSE, TF_ATTR_CONVERTED, L"shunti IME 変換済み"},
    {&GUID_AttrFocused, TF_LS_SOLID, TRUE, TF_ATTR_TARGET_CONVERTED, L"shunti IME 選んでいる文節"},
    // ライブ変換で打ち間違いを直して 1 位にした所 (ユーザー 2026-10-23「一発で直ってるときはわかるように」)。赤橙の波線
    {&GUID_AttrCorrected, TF_LS_SQUIGGLE, FALSE, TF_ATTR_CONVERTED, L"shunti IME 打ち間違いを直した所", RGB(230, 90, 30)},
};
}  // namespace

DisplayAttributeInfo* make_display_attribute(const GUID& guid) {
    for (auto& it : ITEMS)
        if (IsEqualGUID(*it.guid, guid)) return new DisplayAttributeInfo(guid, attr(it.line, it.bold, it.info, it.color), it.desc);
    return nullptr;
}

DisplayAttributeInfo::DisplayAttributeInfo(const GUID& guid, const TF_DISPLAYATTRIBUTE& a, const wchar_t* desc)
    : guid_(guid), attr_(a), desc_(desc) {
    dll_add_ref();
}
DisplayAttributeInfo::~DisplayAttributeInfo() { dll_release(); }

STDMETHODIMP DisplayAttributeInfo::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_INVALIDARG;
    *ppv = nullptr;
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ITfDisplayAttributeInfo)) *ppv = static_cast<ITfDisplayAttributeInfo*>(this);
    if (!*ppv) return E_NOINTERFACE;
    AddRef();
    return S_OK;
}
STDMETHODIMP_(ULONG) DisplayAttributeInfo::AddRef() { return ULONG(InterlockedIncrement(&refs_)); }
STDMETHODIMP_(ULONG) DisplayAttributeInfo::Release() {
    LONG n = InterlockedDecrement(&refs_);
    if (!n) delete this;
    return ULONG(n);
}
STDMETHODIMP DisplayAttributeInfo::GetGUID(GUID* guid) {
    if (!guid) return E_INVALIDARG;
    *guid = guid_;
    return S_OK;
}
STDMETHODIMP DisplayAttributeInfo::GetDescription(BSTR* desc) {
    if (!desc) return E_INVALIDARG;
    *desc = SysAllocString(desc_);
    return *desc ? S_OK : E_OUTOFMEMORY;
}
STDMETHODIMP DisplayAttributeInfo::GetAttributeInfo(TF_DISPLAYATTRIBUTE* a) {
    if (!a) return E_INVALIDARG;
    *a = attr_;
    return S_OK;
}

EnumDisplayAttributeInfo::EnumDisplayAttributeInfo() { dll_add_ref(); }
EnumDisplayAttributeInfo::~EnumDisplayAttributeInfo() { dll_release(); }

STDMETHODIMP EnumDisplayAttributeInfo::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_INVALIDARG;
    *ppv = nullptr;
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IEnumTfDisplayAttributeInfo)) *ppv = static_cast<IEnumTfDisplayAttributeInfo*>(this);
    if (!*ppv) return E_NOINTERFACE;
    AddRef();
    return S_OK;
}
STDMETHODIMP_(ULONG) EnumDisplayAttributeInfo::AddRef() { return ULONG(InterlockedIncrement(&refs_)); }
STDMETHODIMP_(ULONG) EnumDisplayAttributeInfo::Release() {
    LONG n = InterlockedDecrement(&refs_);
    if (!n) delete this;
    return ULONG(n);
}
STDMETHODIMP EnumDisplayAttributeInfo::Clone(IEnumTfDisplayAttributeInfo** out) {
    if (!out) return E_INVALIDARG;
    auto* e = new EnumDisplayAttributeInfo();
    e->index_ = index_;
    *out = e;
    return S_OK;
}
STDMETHODIMP EnumDisplayAttributeInfo::Next(ULONG n, ITfDisplayAttributeInfo** out, ULONG* fetched) {
    if (!out) return E_INVALIDARG;
    ULONG k = 0;
    while (k < n && index_ < ULONG(std::size(ITEMS))) {
        out[k++] = make_display_attribute(*ITEMS[index_++].guid);
    }
    if (fetched) *fetched = k;
    return k == n ? S_OK : S_FALSE;
}
STDMETHODIMP EnumDisplayAttributeInfo::Skip(ULONG n) {
    index_ += n;
    return index_ <= ULONG(std::size(ITEMS)) ? S_OK : S_FALSE;
}

}  // namespace win
}  // namespace shunti
