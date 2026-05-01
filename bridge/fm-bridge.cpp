/* fm-bridge.cpp — C++ bridge between 7z.so and the C API */

#include "StdAfx.h"

#include "Common/MyWindows.h"
#include "Common/MyInitGuid.h"
#include "Common/StringConvert.h"
#include "Windows/DLL.h"
#include "Windows/FileDir.h"
#include "Windows/FileFind.h"
#include "Windows/PropVariant.h"
#include "Windows/PropVariantConv.h"
#include "7zip/Common/FileStreams.h"
#include "7zip/Archive/IArchive.h"
#include "7zip/IPassword.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "fm-bridge-api.h"

/* Zero-then-free for sensitive strings (passwords).
   Uses volatile pointer to prevent the compiler from optimizing out the memset. */
static void secure_free_string(char *s)
{
  if (s) {
    volatile char *p = s;
    size_t len = strlen(s);
    while (len--) *p++ = 0;
    free(s);
  }
}

using namespace NWindows;

Z7_DIAGNOSTIC_IGNORE_CAST_FUNCTION

struct FmBridge;

typedef HRESULT (WINAPI *Func_CreateObject)(const GUID *clsID, const GUID *iid, void **outObject);
typedef HRESULT (WINAPI *Func_GetNumberOfFormats)(UInt32 *numFormats);
typedef HRESULT (WINAPI *Func_GetHandlerProperty2)(UInt32 index, PROPID propID, PROPVARIANT *value);

class COpenCb Z7_final :
  public IArchiveOpenCallback,
  public ICryptoGetTextPassword,
  public CMyUnknownImp
{
  Z7_COM_UNKNOWN_IMP_2(IArchiveOpenCallback, ICryptoGetTextPassword)
  Z7_IFACE_COM7_IMP(IArchiveOpenCallback)
  Z7_IFACE_COM7_IMP(ICryptoGetTextPassword)
public:
  FmBridge *Bridge;
};

Z7_COM7F_IMF(COpenCb::SetTotal(const UInt64 *, const UInt64 *))       { return S_OK; }
Z7_COM7F_IMF(COpenCb::SetCompleted(const UInt64 *, const UInt64 *))   { return S_OK; }

/* ---- extract callback ---- */

class CExtractCb Z7_final :  public IArchiveExtractCallback,
  public ICryptoGetTextPassword,
  public CMyUnknownImp
{
  Z7_COM_UNKNOWN_IMP_2(IArchiveExtractCallback, ICryptoGetTextPassword)
  Z7_IFACE_COM7_IMP(IProgress)
  Z7_IFACE_COM7_IMP(IArchiveExtractCallback)
  Z7_IFACE_COM7_IMP(ICryptoGetTextPassword)
public:
  CMyComPtr<IInArchive> Archive;
  FmBridge *Bridge;
  FString DestDir;
  AString ResolvedDestDir;  /* realpath of DestDir for symlink safety checks */
  FmProgressCb ProgressCb;
  void *UserData;
  UInt64 Total;
  UInt32 ExtractedCount;
  UInt32 SkippedCount;
  COutFileStream *_outFileStreamSpec;
  CMyComPtr<ISequentialOutStream> _outFileStream;

  /* thread-safe copies of bridge state — snapshot at extract start */
  int OverwritePolicy;
  AString CachedPassword;
  FmPasswordCb PasswordCb;
  void *PasswordUserData;
  AString ArchivePath;  /* for password callback context */
  UInt32 ErrorCount;    /* entries that failed during extraction */
};

Z7_COM7F_IMF(CExtractCb::SetTotal(UInt64 total))
{
  Total = total;
  return S_OK;
}

Z7_COM7F_IMF(CExtractCb::SetCompleted(const UInt64 *completed))
{
  if (ProgressCb && completed)
    ProgressCb(*completed, Total, UserData);
  return S_OK;
}

Z7_COM7F_IMF(CExtractCb::PrepareOperation(Int32)) { return S_OK; }
Z7_COM7F_IMF(CExtractCb::SetOperationResult(Int32 opRes))
{
  _outFileStream.Release();
  if (opRes != NArchive::NExtract::NOperationResult::kOK)
    ErrorCount++;
  return S_OK;
}

struct FmBridge
{
  NDLL::CLibrary lib;
  Func_CreateObject         createObject;
  Func_GetNumberOfFormats   getNumFormats;
  Func_GetHandlerProperty2  getHandlerProp2;
  UInt32 numFormats;
  FmPasswordCb passwordCb;
  void *passwordUserData;
  FmOverwriteCb overwriteCb;
  void *overwriteUserData;
  const char *currentArchivePath;
  char *cachedPassword;  /* cached after first successful entry */
  int overwritePolicy;   /* 0=ask(unused now), 1=overwrite all, 2=skip all */
};

struct FmArchive
{
  FmBridge *bridge;
  CMyComPtr<IInArchive> archive;
  UInt32 numItems;
  char formatName[64];
  char *archivePath;  /* persistent copy for password dialogs */
};

static HRESULT bridge_get_password(FmBridge *b, BSTR *password)
{
  if (b && b->cachedPassword) {
    UString us = GetUnicodeString(AString(b->cachedPassword));
    *password = SysAllocString(us);
    return S_OK;
  }
  if (b && b->passwordCb) {
    char *pw = b->passwordCb(b->currentArchivePath, b->passwordUserData);
    if (!pw) return E_ABORT;
    secure_free_string(b->cachedPassword);
    b->cachedPassword = strdup(pw);
    UString us = GetUnicodeString(AString(pw));
    *password = SysAllocString(us);
    secure_free_string(pw);
    return S_OK;
  }
  *password = SysAllocString(L"");
  return S_OK;
}

Z7_COM7F_IMF(COpenCb::CryptoGetTextPassword(BSTR *password))
{ return bridge_get_password(Bridge, password); }

Z7_COM7F_IMF(CExtractCb::CryptoGetTextPassword(BSTR *password))
{
  /* Use the snapshot fields instead of Bridge to avoid races with the main thread */
  if (!CachedPassword.IsEmpty()) {
    UString us = GetUnicodeString(CachedPassword);
    *password = SysAllocString(us);
    return S_OK;
  }
  if (PasswordCb) {
    char *pw = PasswordCb(ArchivePath.Ptr(), PasswordUserData);
    if (!pw) return E_ABORT;
    CachedPassword = pw;
    UString us = GetUnicodeString(AString(pw));
    *password = SysAllocString(us);
    secure_free_string(pw);
    return S_OK;
  }
  *password = SysAllocString(L"");
  return S_OK;
}

/*
  IsSafeRelativePath: reject paths that would escape the extraction root.
  Returns false for:
    - absolute paths (starting with / or \)
    - paths containing ".." that climb above the root
    - paths with embedded null bytes
    - empty paths
*/
static bool IsSafeRelativePath(const UString &path)
{
  if (path.IsEmpty())
    return false;

  /* reject absolute paths */
  if (path[0] == L'/' || path[0] == L'\\')
    return false;

  /* reject embedded nulls */
  for (unsigned i = 0; i < path.Len(); i++)
    if (path[i] == 0)
      return false;

  /* split on / and \ and track directory depth */
  int level = 0;
  unsigned start = 0;
  for (unsigned i = 0; ; i++)
  {
    wchar_t c = (i < path.Len()) ? path[i] : 0;
    if (c == L'/' || c == L'\\' || c == 0)
    {
      unsigned partLen = i - start;
      if (partLen == 2 && path[start] == L'.' && path[start + 1] == L'.')
      {
        level--;
        if (level < 0)
          return false; /* would escape root */
      }
      else if (partLen > 0 && !(partLen == 1 && path[start] == L'.'))
      {
        level++;
      }
      if (c == 0)
        break;
      start = i + 1;
    }
  }
  return true;
}

/*
  IsPathUnderDir: resolve symlinks and verify the result is still under
  resolvedRoot. Prevents symlink escape during extraction.

  For files that don't exist yet, resolves the deepest existing ancestor.
*/
static bool IsPathUnderDir(const char *fsPath, const char *resolvedRoot, size_t rootLen)
{
  /* Try realpath on the path itself first */
  char *resolved = realpath(fsPath, NULL);
  if (!resolved)
  {
    /* Path doesn't exist yet — resolve the parent directory instead.
       This catches the case where a symlink exists as a parent component. */
    AString parent(fsPath);
    int sep = parent.ReverseFind_PathSepar();
    if (sep > 0)
    {
      parent.DeleteFrom((unsigned)sep);
      resolved = realpath(parent.Ptr(), NULL);
    }
    if (!resolved)
    {
      /* Can't resolve at all — the destination root itself may not exist yet.
         If we can't resolve, allow it (CreateComplexDir will handle errors). */
      return true;
    }
  }

  bool safe = (strncmp(resolved, resolvedRoot, rootLen) == 0
    && (resolved[rootLen] == '/' || resolved[rootLen] == '\0'));
  free(resolved);
  return safe;
}

Z7_COM7F_IMF(CExtractCb::GetStream(UInt32 index,
    ISequentialOutStream **outStream, Int32 askExtractMode))
{
  *outStream = NULL;
  if (askExtractMode != NArchive::NExtract::NAskMode::kExtract)
    return S_OK;

  NCOM::CPropVariant prop;
  Archive->GetProperty(index, kpidPath, &prop);
  UString path;
  if (prop.vt == VT_BSTR)
    path = prop.bstrVal;
  else if (prop.vt != VT_EMPTY)
    return E_FAIL;

  if (path.IsEmpty()) {
    /* gzip/bz2/xz wrappers don't store inner filename — derive from archive name */
    if (!ArchivePath.IsEmpty()) {
      AString apath(ArchivePath);
      int sep = apath.ReverseFind_PathSepar();
      if (sep >= 0) apath.DeleteFrontal((unsigned)(sep + 1));
      /* strip outer extension: "file.tar.gz" → "file.tar" */
      int dot = apath.ReverseFind_Dot();
      if (dot >= 0) apath.DeleteFrom((unsigned)dot);
      if (apath.IsEmpty()) apath = "data";
      path = GetUnicodeString(apath);
    } else {
      path = L"data";
    }
  }

  /* Normalize backslashes to forward slashes for consistent checking */
  for (unsigned i = 0; i < path.Len(); i++)
    if (path[i] == L'\\')
      path.ReplaceOneCharAtPos(i, L'/');

  if (!IsSafeRelativePath(path))
  {
    SkippedCount++;
    return S_OK;
  }

  NCOM::CPropVariant propIsDir;
  Archive->GetProperty(index, kpidIsDir, &propIsDir);
  if (propIsDir.vt == VT_BOOL && propIsDir.boolVal != VARIANT_FALSE)
  {
    FString dirPath = DestDir + us2fs(path);
    NFile::NDir::CreateComplexDir(dirPath);
    /* verify the created directory didn't follow a symlink out of DestDir */
    if (!ResolvedDestDir.IsEmpty()
        && !IsPathUnderDir(dirPath.Ptr(), ResolvedDestDir.Ptr(), ResolvedDestDir.Len()))
    {
      SkippedCount++;
      return S_OK;
    }
    return S_OK;
  }

  FString fullPath = DestDir + us2fs(path);

  /* ensure parent directory exists */
  {
    FString dir = fullPath;
    int sep = dir.ReverseFind_PathSepar();
    if (sep >= 0)
    {
      dir.DeleteFrom((unsigned)(sep));
      NFile::NDir::CreateComplexDir(dir);
    }
  }

  /* After creating parent dirs, resolve the actual on-disk path.
     A symlink in the path could cause it to escape DestDir. */
  if (!ResolvedDestDir.IsEmpty()
      && !IsPathUnderDir(fullPath.Ptr(), ResolvedDestDir.Ptr(), ResolvedDestDir.Len()))
  {
    SkippedCount++;
    return S_OK;
  }

  /* check if file exists */
  {
    NFile::NFind::CFileInfo fi;
    if (fi.Find(fullPath)) {
      if (OverwritePolicy == 2) {
        SkippedCount++;
        return S_OK;
      }
    }
  }

  _outFileStreamSpec = new COutFileStream;
  CMyComPtr<ISequentialOutStream> outStreamLoc(_outFileStreamSpec);
  if (!_outFileStreamSpec->Create_ALWAYS(fullPath))
    return E_FAIL;

  _outFileStream = outStreamLoc;
  *outStream = outStreamLoc.Detach();
  ExtractedCount++;
  return S_OK;
}

extern "C" {

FmBridge *fm_bridge_new(const char *lib_path)
{
  FmBridge *b = new FmBridge;
  b->numFormats = 0;
  b->createObject = NULL;
  b->getNumFormats = NULL;
  b->getHandlerProp2 = NULL;
  b->passwordCb = NULL;
  b->passwordUserData = NULL;
  b->overwriteCb = NULL;
  b->overwriteUserData = NULL;
  b->currentArchivePath = NULL;
  b->cachedPassword = NULL;
  b->overwritePolicy = 1;  /* default: overwrite all */

  FString fpath (lib_path);
  if (!b->lib.Load(fpath))
  {
    delete b;
    return NULL;
  }

  b->createObject = Z7_GET_PROC_ADDRESS(Func_CreateObject,
      b->lib.Get_HMODULE(), "CreateObject");
  b->getNumFormats = Z7_GET_PROC_ADDRESS(Func_GetNumberOfFormats,
      b->lib.Get_HMODULE(), "GetNumberOfFormats");
  b->getHandlerProp2 = Z7_GET_PROC_ADDRESS(Func_GetHandlerProperty2,
      b->lib.Get_HMODULE(), "GetHandlerProperty2");

  if (!b->createObject || !b->getNumFormats || !b->getHandlerProp2)
  {
    delete b;
    return NULL;
  }

  b->getNumFormats(&b->numFormats);
  return b;
}

void fm_bridge_free(FmBridge *bridge)
{
  if (bridge) secure_free_string(bridge->cachedPassword);
  delete bridge;
}

FmBridge *fm_bridge_new_auto(void)
{
  FmBridge *b = NULL;

  /* 1. Environment override */
  const char *env = getenv("UNVEILER_PLUGIN_DIR");
  if (env && *env) {
    AString path(env);
    if (path.Back() != CHAR_PATH_SEPARATOR)
      path += CHAR_PATH_SEPARATOR;
    path += "7z.so";
    b = fm_bridge_new(path.Ptr());
    if (b) return b;
  }

  /* 2. Next to the executable */
  {
    char exe[4096];
    ssize_t len = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (len > 0) {
      exe[len] = '\0';
      char *slash = strrchr(exe, '/');
      if (slash) {
        slash[1] = '\0';
        AString path(exe);
        path += "7z.so";
        b = fm_bridge_new(path.Ptr());
        if (b) return b;
      }
    }
  }

  /* 3. Standard install paths */
  b = fm_bridge_new("/usr/local/lib/unveiler/7z.so");
  if (b) return b;
  b = fm_bridge_new("/usr/lib/unveiler/7z.so");
  if (b) return b;

  /* 4. Current directory / LD_LIBRARY_PATH */
  b = fm_bridge_new("7z.so");
  return b;
}

void fm_bridge_set_password_cb(FmBridge *bridge, FmPasswordCb cb, void *user_data)
{
  if (!bridge) return;
  bridge->passwordCb = cb;
  bridge->passwordUserData = user_data;
}

void fm_bridge_set_overwrite_cb(FmBridge *bridge, FmOverwriteCb cb, void *user_data)
{
  if (!bridge) return;
  bridge->overwriteCb = cb;
  bridge->overwriteUserData = user_data;
}

void fm_bridge_set_overwrite_policy(FmBridge *bridge, int policy)
{
  if (bridge) bridge->overwritePolicy = policy;
}

/* helper: try opening with a specific format index */
static FmArchive *try_open_format(FmBridge *bridge, UInt32 fi,
                                  const FString &fpath, UInt64 scanSize)
{
  NCOM::CPropVariant propCls;
  if (bridge->getHandlerProp2(fi, NArchive::NHandlerPropID::kClassID, &propCls) != S_OK)
    return NULL;
  if (propCls.vt != VT_BSTR || SysStringByteLen(propCls.bstrVal) < sizeof(GUID))
    return NULL;

  GUID clsid;
  memcpy(&clsid, propCls.bstrVal, sizeof(GUID));

  CMyComPtr<IInArchive> archive;
  if (bridge->createObject(&clsid, &IID_IInArchive, (void **)&archive) != S_OK)
    return NULL;

  CInFileStream *fileSpec = new CInFileStream;
  CMyComPtr<IInStream> file(fileSpec);
  if (!fileSpec->Open(fpath))
    return NULL;

  COpenCb *openCbSpec = new COpenCb;
  openCbSpec->Bridge = bridge;
  CMyComPtr<IArchiveOpenCallback> openCb(openCbSpec);

  if (archive->Open(file, &scanSize, openCb) != S_OK)
    return NULL;

  FmArchive *a = new FmArchive;
  a->bridge = bridge;
  a->archive = archive;
  a->numItems = 0;
  a->archivePath = strdup(fpath);
  archive->GetNumberOfItems(&a->numItems);

  a->formatName[0] = 0;
  NCOM::CPropVariant propName;
  if (bridge->getHandlerProp2(fi, NArchive::NHandlerPropID::kName, &propName) == S_OK
      && propName.vt == VT_BSTR)
  {
    AString s = UnicodeStringToMultiByte(propName.bstrVal);
    strncpy(a->formatName, s.Ptr(), sizeof(a->formatName) - 1);
    a->formatName[sizeof(a->formatName) - 1] = 0;
  }
  return a;
}

/* check if file extension matches a format's extension list */
static bool ext_matches(FmBridge *bridge, UInt32 fi, const char *path)
{
  NCOM::CPropVariant propExt;
  if (bridge->getHandlerProp2(fi, NArchive::NHandlerPropID::kExtension, &propExt) != S_OK)
    return false;
  if (propExt.vt != VT_BSTR)
    return false;

  /* get file extension(s) from path — handle .tar.gz etc */
  const char *basename = strrchr(path, '/');
  basename = basename ? basename + 1 : path;
  const char *dot = strchr(basename, '.');
  if (!dot) return false;

  AString fileExts = UnicodeStringToMultiByte(propExt.bstrVal);

  /* try each suffix starting from each dot: "file.tar.gz" tries "tar.gz" then "gz" */
  while (dot) {
    const char *ext = dot + 1;
    /* extensions in the property are space-separated */
    AString exts(fileExts);
    const char *p = exts.Ptr();
    while (*p) {
      while (*p == ' ') p++;
      if (!*p) break;
      const char *end = p;
      while (*end && *end != ' ') end++;
      size_t len = (size_t)(end - p);
      if (strlen(ext) == len && strncasecmp(ext, p, len) == 0)
        return true;
      p = end;
    }
    dot = strchr(dot + 1, '.');
  }
  return false;
}

FmArchive *fm_archive_open(FmBridge *bridge, const char *path)
{
  if (!bridge || !path)
    return NULL;

  FString fpath(path);
  const UInt64 scanSize = 1 << 23;

  /* Phase 1: try formats matching the file extension first (no password) */
  secure_free_string(bridge->cachedPassword);
  bridge->cachedPassword = NULL;
  FmPasswordCb savedCb = bridge->passwordCb;
  bridge->passwordCb = NULL;

  for (UInt32 fi = 0; fi < bridge->numFormats; fi++) {
    if (!ext_matches(bridge, fi, path)) continue;
    FmArchive *a = try_open_format(bridge, fi, fpath, scanSize);
    if (a) { bridge->passwordCb = savedCb; return a; }
  }

  /* Phase 2: try all remaining formats (no password) */
  for (UInt32 fi = 0; fi < bridge->numFormats; fi++) {
    if (ext_matches(bridge, fi, path)) continue; /* already tried */
    FmArchive *a = try_open_format(bridge, fi, fpath, scanSize);
    if (a) { bridge->passwordCb = savedCb; return a; }
  }

  /* Phase 3: re-try extension-matching formats WITH password */
  bridge->passwordCb = savedCb;
  if (!bridge->passwordCb)
    return NULL;

  bridge->currentArchivePath = path;

  for (UInt32 fi = 0; fi < bridge->numFormats; fi++) {
    if (!ext_matches(bridge, fi, path)) continue;
    FmArchive *a = try_open_format(bridge, fi, fpath, scanSize);
    if (a) return a;
  }

  return NULL;
}

void fm_archive_close(FmArchive *archive)
{
  if (!archive) return;
  if (archive->archive)
    archive->archive->Close();
  free(archive->archivePath);
  delete archive;
}

uint32_t fm_archive_get_count(FmArchive *archive)
{
  return archive ? archive->numItems : 0;
}

char *fm_archive_get_path(FmArchive *archive, uint32_t index)
{
  if (!archive || index >= archive->numItems)
    return NULL;
  NCOM::CPropVariant prop;
  archive->archive->GetProperty(index, kpidPath, &prop);
  if (prop.vt == VT_BSTR)
  {
    AString s = UnicodeStringToMultiByte(prop.bstrVal);
    return strdup(s.Ptr());
  }
  return NULL;
}

uint64_t fm_archive_get_size(FmArchive *archive, uint32_t index)
{
  if (!archive || index >= archive->numItems)
    return 0;
  NCOM::CPropVariant prop;
  archive->archive->GetProperty(index, kpidSize, &prop);
  if (prop.vt == VT_UI8) return prop.uhVal.QuadPart;
  if (prop.vt == VT_UI4) return prop.ulVal;
  return 0;
}

bool fm_archive_is_dir(FmArchive *archive, uint32_t index)
{
  if (!archive || index >= archive->numItems)
    return false;
  NCOM::CPropVariant prop;
  archive->archive->GetProperty(index, kpidIsDir, &prop);
  return (prop.vt == VT_BOOL && prop.boolVal != VARIANT_FALSE);
}

int64_t fm_archive_get_mtime(FmArchive *archive, uint32_t index)
{
  if (!archive || index >= archive->numItems)
    return 0;
  NCOM::CPropVariant prop;
  archive->archive->GetProperty(index, kpidMTime, &prop);
  if (prop.vt == VT_FILETIME)
  {
    UInt64 ft = ((UInt64)prop.filetime.dwHighDateTime << 32) | prop.filetime.dwLowDateTime;
    return (int64_t)((ft - 116444736000000000ULL) / 10000000ULL);
  }
  return 0;
}

const char *fm_archive_get_format(FmArchive *archive)
{
  return archive ? archive->formatName : NULL;
}

int fm_archive_extract(FmArchive *archive, const uint32_t *indices,
                       uint32_t count, const char *dest_path,
                       FmProgressCb cb, void *user_data)
{
  FmExtractResult r = fm_archive_extract_ex(archive, indices, count, dest_path, cb, user_data);
  return r.ok ? r.extracted : -1;
}

FmExtractResult fm_archive_extract_ex(FmArchive *archive, const uint32_t *indices,
                       uint32_t count, const char *dest_path,
                       FmProgressCb cb, void *user_data)
{
  FmExtractResult result;
  memset(&result, 0, sizeof(result));

  if (!archive || !dest_path) {
    snprintf(result.error_msg, sizeof(result.error_msg), "Invalid archive or destination path");
    return result;
  }

  archive->bridge->currentArchivePath = archive->archivePath;

  CExtractCb *ecbSpec = new CExtractCb;
  CMyComPtr<IArchiveExtractCallback> ecb(ecbSpec);
  ecbSpec->Archive = archive->archive;
  ecbSpec->Bridge = archive->bridge;
  ecbSpec->ProgressCb = cb;
  ecbSpec->UserData = user_data;
  ecbSpec->Total = 0;
  ecbSpec->ExtractedCount = 0;
  ecbSpec->SkippedCount = 0;
  ecbSpec->ErrorCount = 0;

  /* snapshot bridge state for thread safety */
  ecbSpec->OverwritePolicy = archive->bridge->overwritePolicy;
  if (archive->bridge->cachedPassword)
    ecbSpec->CachedPassword = archive->bridge->cachedPassword;
  ecbSpec->PasswordCb = archive->bridge->passwordCb;
  ecbSpec->PasswordUserData = archive->bridge->passwordUserData;
  ecbSpec->ArchivePath = archive->archivePath ? archive->archivePath : "";

  FString dest(dest_path);
  if (dest.Len() > 0 && dest.Back() != FCHAR_PATH_SEPARATOR)
    dest += FCHAR_PATH_SEPARATOR;
  NFile::NDir::CreateComplexDir(dest);
  ecbSpec->DestDir = dest;

  /* resolve the real path of DestDir for symlink safety checks */
  {
    char *resolved = realpath(dest.Ptr(), NULL);
    if (resolved)
    {
      ecbSpec->ResolvedDestDir = resolved;
      free(resolved);
    }
  }

  HRESULT hr;
  if (indices && count > 0)
    hr = archive->archive->Extract(indices, count, 0, ecb);
  else
    hr = archive->archive->Extract(NULL, (UInt32)-1, 0, ecb);

  result.extracted = (int)ecbSpec->ExtractedCount;
  result.skipped   = (int)ecbSpec->SkippedCount;
  result.errors    = (int)ecbSpec->ErrorCount;
  result.ok        = (hr == S_OK);

  if (!result.ok)
    snprintf(result.error_msg, sizeof(result.error_msg),
             "Archive extraction failed (code 0x%08x)", (unsigned)hr);
  else if (result.errors > 0 && result.extracted == 0)
    snprintf(result.error_msg, sizeof(result.error_msg),
             "All files failed to extract (%d errors)", result.errors);
  else if (result.errors > 0)
    snprintf(result.error_msg, sizeof(result.error_msg),
             "%d file%s extracted, %d failed, %d skipped",
             result.extracted, result.extracted == 1 ? "" : "s",
             result.errors, result.skipped);

  return result;
}

} /* extern "C" */
