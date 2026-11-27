#include "../shell_host/ctx_handlers.h"
#include "../shell_host/ctx_menu_icon.h"
#include "../common/windows_compat.h"
#include <cstdio>
#include <fcntl.h>
#include <io.h>
#include <fstream>
void Dump(HMENU menu, IContextMenu2* m2, IContextMenu3* m3, const std::wstring& prefix, int depth = 0) {
    for (int i=0;i<GetMenuItemCount(menu);++i) {
        wchar_t name[512]{}; MENUITEMINFOW item{sizeof(item)};
        item.fMask=MIIM_STRING|MIIM_BITMAP|MIIM_CHECKMARKS|MIIM_SUBMENU|MIIM_ID|MIIM_DATA;
        item.dwTypeData=name; item.cch=511;
        GetMenuItemInfoW(menu, static_cast<UINT>(i), TRUE, &item);
        auto pixels=pulse::shell::MenuItemIcon(menu, static_cast<UINT>(i),m2,m3);
        wprintf(L"raw depth=%d name=%ls bitmap=%p checked=%p unchecked=%p pixels=%ux%u\n",depth,name,item.hbmpItem,item.hbmpChecked,item.hbmpUnchecked,pixels?pixels->width:0,pixels?pixels->height:0);
        if(pixels) {
            std::ofstream f(prefix+L"-"+std::to_wstring(depth)+L"-"+std::to_wstring(i)+L".bgra",std::ios::binary);
            const uint32_t size[]{pixels->width,pixels->height};
            f.write(reinterpret_cast<const char*>(size),sizeof(size));
            f.write(reinterpret_cast<const char*>(pixels->bgra.data()),pixels->bgra.size());
        }
        if(item.hSubMenu && depth<3) Dump(item.hSubMenu,m2,m3,prefix,depth+1);
    }
}
BOOL CALLBACK Resource(HMODULE module,LPCWSTR type,LPWSTR name,LONG_PTR) {
    HRSRC r=FindResourceW(module,name,type); HGLOBAL data=LoadResource(module,r);
    const auto* p=static_cast<const BYTE*>(LockResource(data)); DWORD bytes=SizeofResource(module,r);
    if(type==RT_BITMAP && bytes>=sizeof(BITMAPINFOHEADER)) {
        const auto* h=reinterpret_cast<const BITMAPINFOHEADER*>(p);
        wprintf(L"resource bitmap id=%u dimensions=%ldx%ld bpp=%u\n",IS_INTRESOURCE(name)?LOWORD(reinterpret_cast<ULONG_PTR>(name)):0,h->biWidth,h->biHeight,h->biBitCount);
    }
    if(type==RT_GROUP_ICON && bytes>=6) {
        WORD count=0; memcpy(&count,p+4,2);
        for(WORD i=0;i<count && 6+14u*(i+1)<=bytes;++i) wprintf(L"resource icon group=%u dimensions=%ux%u\n",IS_INTRESOURCE(name)?LOWORD(reinterpret_cast<ULONG_PTR>(name)):0,p[6+14u*i],p[7+14u*i]);
    }
    return TRUE;
}
int wmain(int argc,wchar_t**argv) {
    if(argc!=4)return 2; _setmode(_fileno(stdout),_O_U8TEXT);
    if(wcscmp(argv[1],L"aware")==0)pulse::compat::EnableDpiAwareness();
    HDC dc=GetDC(nullptr); wprintf(L"mode=%ls dpi=%d menucx=%d\n",argv[1],GetDeviceCaps(dc,LOGPIXELSX),GetSystemMetrics(SM_CXMENUCHECK)); ReleaseDC(nullptr,dc);
    CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    pulse::shell::CtxBind bind;
    if(!pulse::shell::BindCtxSelection({argv[2]},false,bind))return 3;
    pulse::shell::CtxHandlerDesc desc; desc.name=L"QQShellExt"; desc.clsid_text=L"{53D2405C-48AB-4C8A-8F59-CE0610F13BBC}";
    CLSIDFromString(desc.clsid_text.c_str(),&desc.clsid);
    pulse::shell::CtxHandlerSlot slot;
    HRESULT hr=pulse::shell::QueryOneHandler(desc,bind,nullptr,1,0x7fff,CMF_NORMAL,slot);
    wprintf(L"QueryOneHandler hr=%08lx\n",hr);
    if(SUCCEEDED(hr)) {
        std::vector<pulse::shell::CtxItemOut> rows; pulse::shell::CollectHandlerItems(slot,false,rows);
        for(const auto& row:rows)wprintf(L"production row=%ls icon=%ux%u id=%u\n",row.text.c_str(),row.icon?row.icon->width:0,row.icon?row.icon->height:0,row.id);
        Dump(slot.hmenu,slot.menu2,slot.menu3,argv[3]);
    }
    wchar_t registered_module[32768]{}; DWORD module_bytes=sizeof(registered_module);
    const auto module_status=RegGetValueW(HKEY_CLASSES_ROOT,
        L"CLSID\\{53D2405C-48AB-4C8A-8F59-CE0610F13BBC}\\InprocServer32", nullptr,
        RRF_RT_REG_SZ|RRF_RT_REG_EXPAND_SZ,nullptr,registered_module,&module_bytes);
    wprintf(L"registered_module=%ls status=%ld\n",registered_module,module_status);
    HMODULE module=module_status==ERROR_SUCCESS ? LoadLibraryExW(registered_module,nullptr,LOAD_LIBRARY_AS_DATAFILE|LOAD_LIBRARY_AS_IMAGE_RESOURCE) : nullptr;
    if(module){EnumResourceNamesW(module,RT_BITMAP,Resource,0);EnumResourceNamesW(module,RT_GROUP_ICON,Resource,0);FreeLibrary(module);}
    for (int group : {129,208,209}) for (uint32_t edge : {16u,24u,32u}) {
        auto p=pulse::common::LoadIconPixels(std::wstring(registered_module)+L",-"+std::to_wstring(group),edge);
        if(p) {
            std::ofstream f(std::wstring(argv[3])+L"-resource-"+std::to_wstring(group)+L"-"+std::to_wstring(edge)+L".bgra",std::ios::binary);
            const uint32_t size[]{p->width,p->height};f.write(reinterpret_cast<const char*>(size),sizeof(size));
            f.write(reinterpret_cast<const char*>(p->bgra.data()),p->bgra.size());
        }
    }
    fflush(stdout); TerminateProcess(GetCurrentProcess(),FAILED(hr)?1:0); return 0;
}
