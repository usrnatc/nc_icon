#include "libnc.h"

#define STB_ASSERT(X) ASSERT(X)
// #define STBI_NO_FAILURE_STRINGS
#define STBI_NO_THREAD_LOCALS
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define STBI_RESIZE_NO_STDIO
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "stb_image_resize2.h"

#define STBI_WRITE_NO_STDIO
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

void
AsyncThreadEntryPoint(void* Params)
{
    LaneContext LaneCtx = *(LaneContext*) Params;

    SetLaneContext(LaneCtx);
    GetTLS()->IS_ASYNC_THREAD = TRUE;
    ThreadSetName("[ASYNC %llu]", LaneIndex());

    for (;;) {
        if (!LaneIndex()) {
            if (!AtomicLoadU32(&ASYNC_LOOP_REPEAT, MEM_ORDER_SEQ_CST)) {
                LOCK_SCOPE(ASYNC_TICK_BEGIN_MTX) {
                    CondVarWait(
                        ASYNC_TICK_BEGIN_COND_VAR, 
                        ASYNC_TICK_BEGIN_MTX, 
                        TimeNow() + SECONDS(1)
                    );
                }
            }

            AtomicExchangeU32(&ASYNC_LOOP_REPEAT, 0, MEM_ORDER_SEQ_CST);
            AtomicExchangeU32(&ASYNC_LOOP_REPEAT_HIGH_PRIORITY, 0, MEM_ORDER_SEQ_CST);
        }

        LaneSync();
        OCAsyncTick();
        CAsyncTick();
        FSAsyncTick();
        LaneSync();

        b32 ShouldQuit = FALSE;

        if (!LaneIndex())
            ShouldQuit = AtomicLoadU32(&GLOBAL_ASYNC_EXIT, MEM_ORDER_SEQ_CST);

        LaneSyncU64(&ShouldQuit, 0);

        if (ShouldQuit)
            break;
    }
}

void
MainThreadBaseEntryPoint(int ArgC, char** ArgV)
{
    ThreadSetName("[MAIN]"_s8);

    TempArena Scratch = GetScratch(NULL, 0);

    ASYNC_TICK_BEGIN_COND_VAR = CondVarAlloc();
    ASYNC_TICK_BEGIN_MTX = MutexAlloc();
    ASYNC_TICK_END_MTX = MutexAlloc();

    Str8List CLIStrings = StrListFromCLIArgs(Scratch.MemPool, ArgC, ArgV);
    CommandLine CLI = CommandLineFromStrList(Scratch.MemPool, CLIStrings);

    OCInit();
    CInit();
    FSInit();

    Handle* AsyncThreads = NULL;
    u64 LaneBroadcastValue = 0;
    u64 MainThreadCount = 1;
    u64 AsyncThreadsCount = GetSystemProperties()->LogicalProcessorCount;
    u64 MainThreadCountClamped = MIN(AsyncThreadsCount, MainThreadCount);

    AsyncThreadsCount -= MainThreadCountClamped;

    Str8 AsyncThreadsCountString = CommandLineString(&CLI, "threads"_s8);

    if (AsyncThreadsCountString.Size)
        AsyncThreadsCount = U64FromStr(AsyncThreadsCountString);

    AsyncThreadsCount = MAX(1, AsyncThreadsCount);

    Handle Barrier = BarrierAlloc(AsyncThreadsCount);
    LaneContext* LaneCtxs = ArenaPushArrayZero(
        Scratch.MemPool, 
        LaneContext, 
        AsyncThreadsCount
    );

    ASYNC_THREADS_COUNT = AsyncThreadsCount;
    AsyncThreads = ArenaPushArrayZero(Scratch.MemPool, Handle, ASYNC_THREADS_COUNT);

    for (u64 Index = 0; Index < ASYNC_THREADS_COUNT; ++Index) {
        LaneCtxs[Index].Index = Index;
        LaneCtxs[Index].Count = ASYNC_THREADS_COUNT;
        LaneCtxs[Index].Barrier = Barrier;
        LaneCtxs[Index].BroadcastMemory = &LaneBroadcastValue;
        AsyncThreads[Index] = ThreadLaunch(AsyncThreadEntryPoint, &LaneCtxs[Index]);
    }

    EntryPoint(&CLI);
    ReleaseScratch(Scratch);
}

struct AppContext {
    Str8 WorkingDir;
    b32 IsDryRun;
    b32 ShouldSkipRename;
    b32 BeVerbose;
    u32 IconSize;
};

PACKED_STRUCT_BEGIN
struct ICOHeader {
    u16 __Reserved0;
    u16 Type;
    u16 Count;
    u8  Width;
    u8  Height;
    u8  ColourCount;
    u8  __Reserved1;
    u16 Planes;
    u16 BitCount;
    u32 BytesInRes;
    u32 ImageOffset;
} PACKED;
PACKED_STRUCT_END

struct PNGWriteContext {
    Arena*    MemPool;
    Str8List* List;
};

struct SharedState {
    AppContext Ctx;
    Str8*      Dirs;
    u64        DirCount;
    u64        RenamedCount;
    u64        SuccessCount;
    u64        FailCount;
    b32        EarlyExit;
};


void 
PNGWriteFunc(void* Ctx, void* Data, int Size)
{
    PNGWriteContext* PNGCtx = (PNGWriteContext*) Ctx;
    Str8 Chunk = ArenaPushStrCpy(
        PNGCtx->MemPool,
        Str((u8*) Data, Size)
    );

    ListPush(PNGCtx->MemPool, PNGCtx->List, Chunk);
}

void
EntryPoint(CommandLine* CLI)
{
    Arena* MemPool = NULL;
    SharedState* Shared = NULL;

    if (!LaneIndex()) {
        MemPool = ArenaAlloc();
        Shared = ArenaPushStructZero(MemPool, SharedState);

        if (CLI->Inputs.Count) {
            Shared->Ctx.WorkingDir = CLI->Inputs.Head->String;
        } else {
            Shared->Ctx.WorkingDir = "."_s8;
        }

        if (Shared->Ctx.WorkingDir.Size) {
            Shared->Ctx.WorkingDir = PathAbsoluteDstFromRelativeDstSrc(
                MemPool,
                Shared->Ctx.WorkingDir,
                GetProcessProperties()->InitialPath
            );
        }

        Shared->Ctx.IsDryRun = (
            CommandLineHasFlag(CLI, "dry-run"_s8) ||
            CommandLineHasFlag(CLI, "d"_s8)
        );

        Str8 IconSizeString = CommandLineString(CLI, "icon-size"_s8);
        if (!IconSizeString.Size)
            IconSizeString = CommandLineString(CLI, "s"_s8);

        if (IconSizeString.Size) {
            Shared->Ctx.IconSize = U32FromStr(IconSizeString);
            if (Shared->Ctx.IconSize != 16 && Shared->Ctx.IconSize != 32 &&
                Shared->Ctx.IconSize != 48 && Shared->Ctx.IconSize != 64 &&
                Shared->Ctx.IconSize != 128 && Shared->Ctx.IconSize != 256) {
                Shared->Ctx.IconSize = 256;
            }
        } else {
            Shared->Ctx.IconSize = 256;
        }

        Shared->Ctx.ShouldSkipRename = CommandLineHasFlag(CLI, "no-rename"_s8);
        Shared->Ctx.BeVerbose = (
            CommandLineHasFlag(CLI, "verbose"_s8) ||
            CommandLineHasFlag(CLI, "v"_s8)
        );

        PrintOut(
            "Enhanced Windows Folder Icon Setter & Renamer\n"
            "===================================================\n"
            "Target Directory: %S\n",
            PRINT_STR(Shared->Ctx.WorkingDir)
        );

        if (Shared->Ctx.IsDryRun)
            PrintOut("Mode: Dry Run (no changes will be made)\n"_s8);

        if (Shared->Ctx.ShouldSkipRename)
            PrintOut("Artwork renaming is DISABLED\n"_s8);

        PrintOut("---------------------------------------------------\n"_s8);

        if (!SystemDirExists(Shared->Ctx.WorkingDir)) {
            PrintErr(
                "ERROR: Directory \"%S\" does not exist\n",
                PRINT_STR(Shared->Ctx.WorkingDir)
            );
            Shared->EarlyExit = TRUE;
        } else {
            Str8List DirsToProcess = {};

            ListPush(MemPool, &DirsToProcess, Shared->Ctx.WorkingDir);

            FileIter* DirIter = SystemFileIterBegin(
                MemPool,
                Shared->Ctx.WorkingDir,
                (
                    SYS_FILE_ITER_RECURSIVE | 
                    SYS_FILE_ITER_SKIP_FILES | 
                    SYS_FILE_ITER_SKIP_HIDDEN
                )
            );

            for (
                FileInfo DirInfo = {}; 
                SystemFileIterNext(MemPool, DirIter, &DirInfo);
            ) {
                ListPush(
                    MemPool, 
                    &DirsToProcess, 
                    ArenaPushStrFmt(
                        MemPool,
                        "%S\\%S",
                        PRINT_STR(DirInfo.Directory),
                        PRINT_STR(DirInfo.Name)
                    )
                );
            }

            SystemFileIterEnd(DirIter);
            Shared->DirCount = DirsToProcess.Count;
            Shared->Dirs = ArenaPushArrayZero(MemPool, Str8, Shared->DirCount);
            
            u64 DirIdx = 0;

            for (Str8Node* Node = DirsToProcess.Head; Node; Node = Node->Next)
                Shared->Dirs[DirIdx++] = Node->String;
        }
    }

    LaneSyncU64(&Shared, 0);

    if (Shared->EarlyExit) {
        if (!LaneIndex()) 
            ArenaRelease(MemPool);

        return;
    }

    struct {
        Str8 Pattern;
        Str8 Replacement;
    } RenameRules[] = {
        {"-poster."_s8, "folder"_s8},
        {"-clearlogo."_s8, "logo"_s8},
        {"-fanart."_s8, "fanart"_s8},
        {"-disc."_s8, "disc"_s8},
        {"-banner."_s8, "landscape"_s8},
        {"cover_art."_s8, "folder"_s8}
    };

    r1u64 Range = LaneRange(Shared->DirCount);

    for (u64 Index = Range.Min; Index < Range.Max; ++Index) {
        TempArena Scratch = GetScratch(NULL, 0);
        Str8 CurrentFolder = Shared->Dirs[Index];
        Str8 DesktopINIPath = ArenaPushStrFmt(
            Scratch.MemPool,
            "%S\\desktop.ini",
            PRINT_STR(CurrentFolder)
        );
        b32 HasDesktopINI = SystemFileExists(DesktopINIPath);
        Str8 FoundPosterPath = {};
        FileIter* Iter = SystemFileIterBegin(
            Scratch.MemPool,
            CurrentFolder,
            SYS_FILE_ITER_SKIP_DIRS | SYS_FILE_ITER_SKIP_HIDDEN
        );

        for (FileInfo Info = {}; SystemFileIterNext(Scratch.MemPool, Iter, &Info);) {
            TempArena Scratch2 = GetScratch(&Scratch.MemPool, 1);
            Str8 FileName = Info.Name;
            Str8 Extension = StrSkipLastDot(FileName);
            Str8 CurrentFullPath = ArenaPushStrFmt(
                Scratch2.MemPool,
                "%S\\%S",
                PRINT_STR(CurrentFolder),
                PRINT_STR(FileName)
            );

            if (!Shared->Ctx.ShouldSkipRename) {
                for (u64 RuleIdx = 0; RuleIdx < ARRAY_COUNT(RenameRules); ++RuleIdx) {
                    Str8 Pattern = RenameRules[RuleIdx].Pattern;
                    Str8 Replacement = RenameRules[RuleIdx].Replacement;
                    u64 PatternLoc = StrFindSubStr(
                        FileName,
                        Pattern,
                        0,
                        STR_MATCH_ALL_CASES | STR_MATCH_LAST
                    );

                    if (PatternLoc != FileName.Size) {
                        Str8 NewFullPath = ArenaPushStrFmt(
                            Scratch2.MemPool,
                            "%S\\%S.%S",
                            PRINT_STR(CurrentFolder),
                            PRINT_STR(Replacement),
                            PRINT_STR(Extension)
                        );

                        if (SystemFileExists(NewFullPath)) {
                            if (Shared->Ctx.BeVerbose) {
                                PrintOut(
                                    "\t- Skip rename (target exists): \"%S.%S\"\n",
                                    PRINT_STR(Replacement),
                                    PRINT_STR(Extension)
                                );
                            }
                        } else {
                            if (Shared->Ctx.IsDryRun) {
                                PrintOut(
                                    "[DRY RUN] Would rename \"%S\" -> \"%S.%S\"\n",
                                    PRINT_STR(FileName),
                                    PRINT_STR(Replacement),
                                    PRINT_STR(Extension)
                                );
                            } else {
                                if (Shared->Ctx.BeVerbose) {
                                    PrintOut(
                                        "Renaming \"%S\" -> \"%S.%S\"\n",
                                        PRINT_STR(FileName),
                                        PRINT_STR(Replacement),
                                        PRINT_STR(Extension)
                                    );
                                }

                                if (SystemMoveFile(NewFullPath, CurrentFullPath)) {
                                    AtomicAddFetchU64(&Shared->RenamedCount, 1, MEM_ORDER_SEQ_CST);
                                    FileName = ArenaPushStrFmt(
                                        Scratch2.MemPool,
                                        "%S.%S",
                                        PRINT_STR(Replacement),
                                        PRINT_STR(Extension)
                                    );
                                    CurrentFullPath = NewFullPath;
                                } else {
                                    PrintOut("✗ Error renaming \"%S\"\n", PRINT_STR(FileName));
                                }
                            }
                        }
                        break;
                    }
                }
            }

            if (!HasDesktopINI && !FoundPosterPath.Size) {
                Str8 NameNoExt = StrChopLastDot(FileName);
                if (StrMatch(NameNoExt, "folder"_s8, STR_MATCH_ALL_CASES) ||
                    StrMatch(NameNoExt, "poster"_s8, STR_MATCH_ALL_CASES)) {
                    if (StrMatch(Extension, "jpg"_s8, STR_MATCH_ALL_CASES) ||
                        StrMatch(Extension, "jepg"_s8, STR_MATCH_ALL_CASES) ||
                        StrMatch(Extension, "png"_s8, STR_MATCH_ALL_CASES)) {
                        FoundPosterPath = ArenaPushStrCpy(Scratch.MemPool, CurrentFullPath);
                    }
                }
            }
            ReleaseScratch(Scratch2);
        }
        SystemFileIterEnd(Iter);

        if (FoundPosterPath.Size && !HasDesktopINI) {
            PrintOut("\nProcessing folder: %S\n", PRINT_STR(CurrentFolder));

            Str8 IconOutputPath = ArenaPushStrFmt(
                Scratch.MemPool,
                "%S\\folder.ico",
                PRINT_STR(CurrentFolder)
            );

            if (Shared->Ctx.IsDryRun) {
                PrintOut("\t[DRY RUN] Would create icon from: %S\n", PRINT_STR(FoundPosterPath));
                AtomicAddFetchU64(&Shared->SuccessCount, 1, MEM_ORDER_SEQ_CST);
            } else {
                b32 IconSuccess = FALSE;
                Str8 FileData = ReadFileContents(Scratch.MemPool, FoundPosterPath);

                if (FileData.Size) {
                    int Width, Height, Channels;
                    u8* Pixels = stbi_load_from_memory(
                        FileData.Str,
                        (int)FileData.Size,
                        &Width,
                        &Height,
                        &Channels,
                        4
                    );

                    if (Pixels) {
                        int TargetSize = (int)Shared->Ctx.IconSize;
                        f32 Ratio = (f32)Width / (f32)Height;
                        int NewWidth = TargetSize;
                        int NewHeight = TargetSize;

                        if (Ratio > 1.0f) {
                            NewHeight = (int)(TargetSize / Ratio);
                        } else {
                            NewWidth = (int)(TargetSize * Ratio);
                        }

                        NewWidth = MAX(1, NewWidth);
                        NewHeight = MAX(1, NewHeight);

                        u8* ResizedPixels = ArenaPushArrayZero(Scratch.MemPool, u8, NewWidth * NewHeight * 4);
                        u8* BackgroundPixels = ArenaPushArrayZero(Scratch.MemPool, u8, TargetSize * TargetSize * 4);

                        stbir_resize_uint8_srgb(
                            Pixels, Width, Height, 0,
                            ResizedPixels, NewWidth, NewHeight, 0,
                            STBIR_RGBA
                        );

                        int OffsetX = (TargetSize - NewWidth) / 2;
                        int OffsetY = (TargetSize - NewHeight) / 2;

                        for (int Y = 0; Y < NewHeight; ++Y) {
                            u8* DstRow = BackgroundPixels + ((OffsetY + Y) * TargetSize + OffsetX) * 4;
                            u8* SrcRow = ResizedPixels + (Y * NewWidth) * 4;
                            MemCpy(DstRow, SrcRow, NewWidth * 4);
                        }

                        Str8List PNGDataList = {};
                        PNGWriteContext Ctx = { Scratch.MemPool, &PNGDataList };

                        stbi_write_png_to_func(
                            PNGWriteFunc, &Ctx,
                            TargetSize, TargetSize, 4,
                            BackgroundPixels, TargetSize * 4
                        );

                        ICOHeader Header = {};
                        Header.Type = 1;
                        Header.Count = 1;
                        Header.Width = (TargetSize == 256) ? 0 : (u8)TargetSize;
                        Header.Height = (TargetSize == 256) ? 0 : (u8)TargetSize;
                        Header.Planes = 1;
                        Header.BitCount = 32;
                        Header.BytesInRes = (u32)PNGDataList.TotalSize;
                        Header.ImageOffset = sizeof(ICOHeader);

                        Str8List ICOList = {};
                        ListPush(Scratch.MemPool, &ICOList, Str8Struct(&Header));
                        ListPushCat(&ICOList, &PNGDataList);
                        
                        LogInfo("writing icon file %S", PRINT_STR(IconOutputPath));
                        IconSuccess = WriteFileContents(IconOutputPath, ICOList);
                        stbi_image_free(Pixels);
                    } else {
                        PrintOut("✗ Failed to decode image: %S\n", PRINT_STR(FoundPosterPath));
                    }
                } else {
                    LogError("failed to open poster image file \"%S\"", PRINT_STR(FoundPosterPath));
                }

                if (IconSuccess) {
                    Str8 INIContent = "[.ShellClassInfo]\r\nIconResource=folder.ico,0\r\n"_s8;
                    if (WriteFileContents(DesktopINIPath, INIContent)) {
                        Str16 WideINIPath = Str16FromStr8(Scratch.MemPool, DesktopINIPath);
                        Str16 WideFolderPath = Str16FromStr8(Scratch.MemPool, CurrentFolder);

                        SetFileAttributesW((WCHAR*)WideINIPath.Str, FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_HIDDEN);
                        SetFileAttributesW((WCHAR*)WideFolderPath.Str, FILE_ATTRIBUTE_READONLY);
                        
                        AtomicAddFetchU64(&Shared->SuccessCount, 1, MEM_ORDER_SEQ_CST);
                    } else {
                        LogError("failed to write INI file"_s8);
                        AtomicAddFetchU64(&Shared->FailCount, 1, MEM_ORDER_SEQ_CST);
                    }
                } else {
                    LogError("failed to write ICO file"_s8);
                    AtomicAddFetchU64(&Shared->FailCount, 1, MEM_ORDER_SEQ_CST);
                }
            }
        }
        ReleaseScratch(Scratch);
    }

    // 4. Sync and Teardown on Lane 0
    LaneSync();

    if (!LaneIndex()) {
        if (!Shared->Ctx.IsDryRun && Shared->SuccessCount > 0) {
            SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, NULL, NULL);
            PrintOut("✓ Notified Windows Explorer to refresh icons.\n"_s8);
        }

        PrintOut("\n===================================================\n"_s8);
        PrintOut("Icon creation complete:\n"_s8);
        PrintOut("\tSuccessfully configured folders: %llu\n", Shared->SuccessCount);
        PrintOut("\tFailed configurations: %llu\n", Shared->FailCount);
        PrintOut("\tTotal unique folders processed: %llu\n", Shared->DirCount);

        ArenaRelease(MemPool);
    }
}
