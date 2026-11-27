/*
 * The PC port's pause menu, for native builds only (the port compiles it as
 * an extra unit of the main image): Esc, or a controller's Guide button,
 * freezes the game and opens the game's own option dialogs over its last
 * picture, dimmed: Resume, Options (the host's settings, in a panel of the
 * game's options' look for each category), Quit to title (the game's soft
 * reset), Quit game, the last two confirmed with a Yes and No of its own.
 *
 * While it is open the main loop runs no game task: each frame runs only
 * the menu's own task list, draws its ordering table over the saved
 * picture and shows it (gamemain.c calls pcMenuFrame); it opens only where
 * the game loop draws the frames (gameplay, the title screen), not over
 * movies. The host asks for it and quits the game (the host's
 * pause_menu.rs).
 */
#ifdef PC_PORT

#include <psyq/sys/types.h>
#include <psyq/libgte.h>
#include <psyq/libgpu.h>

#include "common.h"
#include "main/display.h"
#include "main/display_types.h"
#include "main/fs.h"
#include "main/mc.h"
#include "main/mem.h"
#include "main/pad.h"
#include "main/sound.h"
#include "main/sound_ids.h"
#include "main/task.h"
#include "main/text.h"
#include "main/ui.h"
#include "main/ui_types.h"
#include "main/session.h"
#include "main/wipsys.h"
#include "gameplay/companion_load.h"
#include "../display.h"
#include "../fs.h"
#include "../mc.h"
#include "../task.h"

/* The host's side (pause_menu.rs). */
extern void pc_menu_available(s32 available);
extern s32 pc_menu_requested(void);
extern void pc_menu_taken(void);
extern s32 pc_menu_confirms_quit(void);
extern void pc_menu_quit(void);
/* The debug menu is on ([debug] menu, PE2_DEBUG_MENU; pause_menu.rs). */
extern s32 pc_debug_menu(void);

/* The host's settings (settings.rs): categories, their rows, each row's
 * label and choices (NULL past the last; -1 the current one). */
extern s32       pc_options_categories(void);
extern const u8* pc_options_category(s32 category, s32 title);
extern s32       pc_options_count(s32 category);
extern const u8* pc_options_label(s32 category, s32 row);
extern const u8* pc_options_choice(s32 category, s32 row, s32 choice);
extern s32       pc_options_needs_restart(s32 category, s32 row, s32 ever);
extern const u8* pc_options_restart_text(void);
extern void      pc_options_change(s32 category, s32 row, s32 delta);
extern void      pc_options_save(void);

/* ui.c: the shared option-dialog state, kept across the menu. */
extern void pcUiSaveState(void);
extern void pcUiRestoreState(void);
extern void pcUiSelectDialogRow(s32 row);

enum {
    PC_MENU_CLOSED,
    PC_MENU_PAUSE,
    PC_MENU_CONFIRM_TITLE,
    PC_MENU_CONFIRM_QUIT,
    PC_MENU_CATEGORIES,
    PC_MENU_SETTINGS,
    PC_MENU_SAVE,
    PC_MENU_DEBUG,
};

/* What happens once the menu's dialogs have closed. */
enum {
    PC_MENU_THEN_NOTHING,
    PC_MENU_THEN_PAUSE,
    PC_MENU_THEN_CONFIRM_TITLE,
    PC_MENU_THEN_CONFIRM_QUIT,
    PC_MENU_THEN_RESUME,
    PC_MENU_THEN_TITLE,
    PC_MENU_THEN_QUIT,
    PC_MENU_THEN_CATEGORIES,
    PC_MENU_THEN_SETTINGS,
    PC_MENU_THEN_SAVE,
    PC_MENU_THEN_DEBUG,
    PC_MENU_THEN_WARP,
};

static u8 _gPcMenuPauseTitle[]    = "PAUSE";
static u8 _gPcMenuTitleTitle[]    = "QUIT TO TITLE";
static u8 _gPcMenuQuitTitle[]     = "QUIT GAME";
static u8 _gPcMenuResumeText[]    = "Resume";
static u8 _gPcMenuSaveText[]      = "Save";
/* PE2PC's texts in the game's screens (item_menu.c), listed here with the
 * menu's for translators. */
const u8 gPcTextMovedToBox[]  = "Moved to the item box.";
const u8 gPcTextQuantity[]    = "Quantity";
const u8 gPcTextSetNumber[]   = "Set the number with the directional\nbuttons. } button to confirm.";
static u8 _gPcMenuOptionsText[]   = "Options";
static u8 _gPcMenuOptionsTitle[]  = "OPTIONS";
static u8 _gPcMenuToTitleText[]   = "Quit to title";
static u8 _gPcMenuQuitGameText[]  = "Quit game";
/* The confirmations' answers: as the game's (main/mc.c), but PE2PC's own,
 * so they follow PE2PC's language and not the disc's. */
static u8 _gPcMenuNoText[]        = "No";
static u8 _gPcMenuYesText[]       = "Yes";

static TaskNode              _gPcMenuTasks;
static UiOptionDialogRequest _gPcMenuRequest;
static UiDialogOption        _gPcMenuOptions[8];
static UiObject*             _gPcMenuDialog;
static s32                   _gPcMenuState;
static s32                   _gPcMenuThen;
/* What each row of the pause dialog does (its rows depend on the game's state). */
static s32 _gPcMenuPauseRows[6];
/* A settings panel's view: the category shown, its row list, where a row's
 * choice is drawn and each row's widest choice (content pixels). Two: the
 * pause menu's, and the game's Option list's (pcOptionsUpdateSettingsRow),
 * which the pause menu may open over. */
typedef struct {
    s32    category;
    UiList list;
    s32    choiceX;
    s32    rowChoiceWidth[16];
} PcMenuSettingsView;
enum { PC_MENU_VIEW_PAUSE, PC_MENU_VIEW_OPTIONS };
static PcMenuSettingsView _gPcMenuViews[2];
// The menus' font has no < or >: its guillemets point instead.
static u8        _gPcMenuCaretLeft[]  = "\xAB";
static u8        _gPcMenuCaretRight[] = "\xBB";
// A caret and the gap after it, in pixels; measured with the panel.
static s32       _gPcMenuCaretWidth;
enum { PC_MENU_CARET_GAP_PIXELS = 3 };
static UiObject* _gPcMenuPanel;
static void      _pcMenuDrawSettingRow(UiList* list, UiObject* object);
static UiListRowCallback _gPcMenuSettingRowCallbacks[] = { _pcMenuDrawSettingRow };
/* The game's last picture, shown dimmed behind the menu. */
static u16      _gPcMenuScreen[DISPLAY_SCREEN_WIDTH * DISPLAY_SCREEN_HEIGHT];
static TILE     _gPcMenuDimTile;
static DR_TPAGE _gPcMenuDimMode;
/* Quit to title asked, for gamemain.c's soft reset at the top of its loop. */
static s32 _gPcMenuTitleRequested;
/* Save anywhere ([gameplay] save_anywhere): the main-loop update gameplay
 * last found the player free to save in (pad_input.c), what it does before
 * and after a save, and the save dialog (main/mc.c's) while open. */
static u32       _gPcMenuSaveAllowedAt = (u32)-2;
static void      (*_gPcMenuSavePrepare)(void);
static void      (*_gPcMenuSaveDone)(void);
static UiObject* _gPcMenuSave;

/// Whether the game was free to save in the update before the menu opened.
static s32 _pcMenuCanSave(void)
{
    return gDisplayState.loopCount - _gPcMenuSaveAllowedAt <= 1;
}

/// Whether quitting to the title is possible: what the game's soft reset
/// needs (gamemain.c).
static s32 _pcMenuCanQuitToTitle(void)
{
    return gDisplayState.gameMode == DISPLAY_GAME_ACTIVE && gDisplayState.gameRunning != 0;
}

/// Widens `panel` (kept centred) to hold its title: the game's dialogs fit
/// their options only, its titles being short. The title is drawn in the
/// small face from three pixels in, on a tab a little wider.
static void _pcMenuFitTitle(UiPanel* panel, const u8* title)
{
    enum { PC_MENU_TITLE_MARGIN_PIXELS = 16 };
    TextDrawReq request;
    s32         wanted;

    request.glyphTable = TEXT_GLYPH_TABLE_SMALL;
    request.alignment  = TEXT_ALIGNMENT_RIGHT;
    request.x          = 0;
    request.y          = 0;
    textAlignLine(&request, title);
    wanted = -request.x + PC_MENU_TITLE_MARGIN_PIXELS;
    if ((s16)panel->bounds.unsignedRect.w < wanted) {
        panel->bounds.unsignedRect.w = wanted;
        panel->bounds.unsignedRect.x = -(wanted / 2);
    }
}

/// Opens an option dialog on the menu's task list, its cursor on `selected`.
static void _pcMenuOpenDialog(u8* title, u8** labels, s32 count, s32 selected)
{
    TaskNode* previous;
    s32       i;

    for (i = 0; i < count; i++) {
        _gPcMenuOptions[i].text = labels[i];
        _gPcMenuOptions[i].next = i + 1 < count ? &_gPcMenuOptions[i + 1] : NULL;
    }
    _gPcMenuRequest.optionCount = count;
    _gPcMenuRequest.options     = _gPcMenuOptions;
    _gPcMenuRequest.title       = (char*)title;
    _gPcMenuRequest.flags       = USER_INTERFACE_OPTION_DIALOG_CANCELLABLE;
    previous                    = taskGetActiveList();
    taskSetActiveList(&_gPcMenuTasks);
    _gPcMenuDialog = uiSpawnOptionDialog(&_gPcMenuRequest, 0, 0, 0);
    taskSetActiveList(previous);
    pcUiSelectDialogRow(selected);
    if (_gPcMenuDialog != NULL) {
        _pcMenuFitTitle(&_gPcMenuDialog->panel, title);
    }
}

/* ---- The debug menu ([debug] menu): a panel of the pause menu's for
 * testing. Stage, Room and Entrance pick a place (Left and Right), Go to
 * room travels there as the game's own scripted transitions do (the live
 * save's location, then the session reload task); the other rows act at
 * once. The rooms are every room folder with a warp table, generated from
 * include/main/areas.h and the rooms' symbol maps. ---- */

typedef struct {
    u8          stage;
    u8          area;
    u8          entrances; // The room's warp table entries (the arrival points)
    const char* name;
} PcDebugRoom;

static const PcDebugRoom _gPcDebugRooms[] = {
#include "debug_rooms.inc"
};

static u8 _gPcMenuDebugText[]  = "Debug";
static u8 _gPcMenuDebugTitle[] = "DEBUG";

enum {
    PC_DEBUG_ROW_STAGE,
    PC_DEBUG_ROW_ROOM,
    PC_DEBUG_ROW_ENTRANCE,
    PC_DEBUG_ROW_GO,
    PC_DEBUG_ROW_REFILL,
    PC_DEBUG_ROW_CURE,
    PC_DEBUG_ROW_BP,
    PC_DEBUG_ROW_EXP,
    PC_DEBUG_ROW_COUNT,
};

static const char* const _gPcDebugRowLabels[PC_DEBUG_ROW_COUNT] = {
    "Stage", "Room", "Entrance", "Go to room", "Refill HP/MP", "Cure status", "BP +10000", "EXP +10000",
};

static const char* const _gPcDebugStageNames[] = {
    "", "Akropolis", "Dryfield", "Dryfield (night)", "Mine and Shelter", "Shelter and Neo Ark",
};

/* The place picked (an index into _gPcDebugRooms, and a zero-based
 * entrance), the panel, its row list and where its choices are drawn. */
static s32       _gPcDebugRoom;
static s32       _gPcDebugEntrance;
static UiObject* _gPcMenuDebugPanel;
static UiList    _gPcDebugList;
static s32       _gPcDebugChoiceX;
static s32       _gPcDebugGo;
static u8        _gPcDebugEntranceText[8];
static void      _pcMenuDrawDebugRow(UiList* list, UiObject* object);
static UiListRowCallback _gPcDebugRowCallbacks[] = { _pcMenuDrawDebugRow };

/// Whether the debug menu shows: on, and in a game (gameplay loaded, a stage).
static s32 _pcMenuCanDebug(void)
{
    return pc_debug_menu() && gDisplayState.gameMode == DISPLAY_GAME_ACTIVE && gDisplayState.gameRunning != 0 &&
           gGameSession != NULL && gGameSession->location.stage != 0;
}

/// The picked room's index moved by `delta` within its stage, wrapping.
static s32 _pcDebugStepRoom(s32 room, s32 delta)
{
    s32 count;
    s32 stage;

    count = ARRAY_SIZE(_gPcDebugRooms);
    stage = _gPcDebugRooms[room].stage;
    do {
        room = (room + delta + count) % count;
    } while (_gPcDebugRooms[room].stage != stage);
    return room;
}

/// The first room of the stage `delta` stages on, wrapping.
static s32 _pcDebugStepStage(s32 room, s32 delta)
{
    s32 count;
    s32 stage;
    s32 i;

    count = ARRAY_SIZE(_gPcDebugRooms);
    stage = _gPcDebugRooms[room].stage;
    stage = (stage - 1 + delta + 5) % 5 + 1;
    for (i = 0; i < count; i++) {
        if (_gPcDebugRooms[i].stage == stage) {
            return i;
        }
    }
    return room;
}

/// The row's current choice, as text.
static const u8* _pcDebugChoice(s32 row)
{
    const PcDebugRoom* room;
    s32                n;
    s32                i;

    room = &_gPcDebugRooms[_gPcDebugRoom];
    switch (row) {
        case PC_DEBUG_ROW_STAGE:
            return (const u8*)_gPcDebugStageNames[room->stage];
        case PC_DEBUG_ROW_ROOM:
            return (const u8*)room->name;
        case PC_DEBUG_ROW_ENTRANCE:
            // "N of M", without printf.
            i = 0;
            n = _gPcDebugEntrance + 1;
            if (n >= 10) {
                _gPcDebugEntranceText[i++] = '0' + n / 10;
            }
            _gPcDebugEntranceText[i++] = '0' + n % 10;
            _gPcDebugEntranceText[i++] = '/';
            n = room->entrances;
            if (n >= 10) {
                _gPcDebugEntranceText[i++] = '0' + n / 10;
            }
            _gPcDebugEntranceText[i++] = '0' + n % 10;
            _gPcDebugEntranceText[i]   = 0;
            return _gPcDebugEntranceText;
    }
    return NULL;
}

/// A row's Confirm: the actions (Go to room closes the menu).
static void _pcDebugAct(UiObject* object, s32 row)
{
    switch (row) {
        case PC_DEBUG_ROW_GO:
            _gPcDebugGo    = 1;
            object->result = USER_INTERFACE_RESULT_CONFIRM;
            break;
        case PC_DEBUG_ROW_REFILL:
            gPlayerStatus.hp = gPlayerStatus.hpMax;
            gPlayerStatus.mp = gPlayerStatus.mpMax;
            break;
        case PC_DEBUG_ROW_CURE:
            gPlayerStatus.statusFlags = 0;
            break;
        case PC_DEBUG_ROW_BP:
            gPlayerStatus.bp += 10000;
            break;
        case PC_DEBUG_ROW_EXP:
            gPlayerStatus.exp += 10000;
            break;
    }
}

/// Draws a debug row: its label, and for the pickers their choice between
/// carets on the selected row (Left and Right change it); Confirm acts.
static void _pcMenuDrawDebugRow(UiList* list, UiObject* object)
{
    s32       row;
    s32       x;
    s32       y;
    s32       delta;
    u32       dim;
    const u8* choice;
    const PcDebugRoom* room;

    row    = list->currentItemIndex;
    y      = list->rowTextY.signedValue;
    x      = object->panel.contentLeft.signedValue;
    dim    = uiGetTextColor(object, USER_INTERFACE_TEXT_COLOR_DIMMED);
    textDrawUiLine(object, x + 6, y, (const u8*)_gPcDebugRowLabels[row], list->colorRgb, TEXT_DRAW_OUTLINED, TEXT_ALIGNMENT_LEFT);
    choice = _pcDebugChoice(row);
    if (choice != NULL) {
        x += _gPcDebugChoiceX;
        if (list->selectedItemIndex == row) {
            textDrawUiLine(object, x, y, _gPcMenuCaretLeft, dim, TEXT_DRAW_OUTLINED, TEXT_ALIGNMENT_LEFT);
            textDrawUiLine(object, x + _gPcMenuCaretWidth + textMeasureLineWidth(choice) + PC_MENU_CARET_GAP_PIXELS, y,
                           _gPcMenuCaretRight, dim, TEXT_DRAW_OUTLINED, TEXT_ALIGNMENT_LEFT);
        }
        textDrawUiLine(object, x + _gPcMenuCaretWidth, y, choice, uiGetTextColor(object, USER_INTERFACE_TEXT_COLOR_NORMAL),
                       TEXT_DRAW_OUTLINED, TEXT_ALIGNMENT_LEFT);
    }
    if (list->rowInputEnabled != USER_INTERFACE_LIST_ROW_ACTIVE) {
        return;
    }
    delta = 0;
    if (padCheckButtons(0, PAD_BUTTON_QUERY_PRESSED, PAD_BUTTON_RIGHT) != 0) {
        delta = 1;
    } else if (padCheckButtons(0, PAD_BUTTON_QUERY_PRESSED, PAD_BUTTON_LEFT) != 0) {
        delta = -1;
    }
    if (delta != 0 && choice != NULL) {
        sndEvtRequestScriptStart(SOUND_MENU_CURSOR, 0, 0);
        switch (row) {
            case PC_DEBUG_ROW_STAGE:
                _gPcDebugRoom     = _pcDebugStepStage(_gPcDebugRoom, delta);
                _gPcDebugEntrance = 0;
                break;
            case PC_DEBUG_ROW_ROOM:
                _gPcDebugRoom     = _pcDebugStepRoom(_gPcDebugRoom, delta);
                _gPcDebugEntrance = 0;
                break;
            case PC_DEBUG_ROW_ENTRANCE:
                room              = &_gPcDebugRooms[_gPcDebugRoom];
                _gPcDebugEntrance = (_gPcDebugEntrance + delta + room->entrances) % room->entrances;
                break;
        }
    } else if (choice == NULL && padCheckButtons(0, PAD_BUTTON_QUERY_PRESSED, gPadConfirmButtonMask) != 0) {
        sndEvtRequestScriptStart(SOUND_MENU_CONFIRM, 0, 0);
        _pcDebugAct(object, row);
    }
}

/// The debug panel's task, laid out as the settings panel's: fitted on its
/// first update to the labels and the widest room name; Cancel publishes a
/// confirm for the menu to close it.
static void _pcMenuUpdateDebugTask(Task* owningTask)
{
    enum {
        PC_DEBUG_ROW_HEIGHT_PIXELS = 16,
        PC_DEBUG_COLUMN_GAP_PIXELS     = 16,
        PC_DEBUG_COLUMN_GAP_MIN_PIXELS = 6,
        PC_DEBUG_SCREEN_MARGIN_PIXELS  = 8,
    };
    UiObject* object;
    UiList*   list;
    s32       i;
    s32       width;
    s32       labelWidth;
    s32       choiceWidth;
    s32       rowWidth;
    s32       widest;
    s32       gap;

    object = owningTask->spawnArg2.pointer;
    list   = &_gPcDebugList;
    if (owningTask->state == 0) {
        list->rowCallbacks                  = _gPcDebugRowCallbacks;
        list->itemCount                     = PC_DEBUG_ROW_COUNT;
        list->visibleRowCount.unsignedValue = PC_DEBUG_ROW_COUNT;
        list->wrapNavigation                = 0;
        list->rowHeight                     = PC_DEBUG_ROW_HEIGHT_PIXELS;
        list->selectedItemIndex             = 0;
        uiFitPanelToList(list, &object->panel);
        list->flags        = USER_INTERFACE_LIST_SHARED_ROW_CALLBACK;
        _gPcMenuCaretWidth = textMeasureLineWidth(_gPcMenuCaretLeft) + PC_MENU_CARET_GAP_PIXELS;
        labelWidth         = 0;
        for (i = 0; i < PC_DEBUG_ROW_COUNT; i++) {
            width = textMeasureLineWidth((const u8*)_gPcDebugRowLabels[i]);
            if (labelWidth < width) {
                labelWidth = width;
            }
        }
        choiceWidth = 0;
        for (i = 0; i < (s32)ARRAY_SIZE(_gPcDebugRooms); i++) {
            width = textMeasureLineWidth((const u8*)_gPcDebugRooms[i].name);
            if (choiceWidth < width) {
                choiceWidth = width;
            }
        }
        for (i = 1; i < (s32)ARRAY_SIZE(_gPcDebugStageNames); i++) {
            width = textMeasureLineWidth((const u8*)_gPcDebugStageNames[i]);
            if (choiceWidth < width) {
                choiceWidth = width;
            }
        }
        // No wider than the 4:3 screen (with the game's eight-pixel margins),
        // as the settings panel: the gap between the columns narrows first.
        widest = DISPLAY_SCREEN_WIDTH - 2 * PC_DEBUG_SCREEN_MARGIN_PIXELS -
                 (object->panel.bounds.rect.w - (object->panel.contentRight.signedValue - object->panel.contentLeft.signedValue));
        rowWidth = 2 * _gPcMenuCaretWidth + choiceWidth;
        gap      = PC_DEBUG_COLUMN_GAP_PIXELS;
        if (6 + labelWidth + gap + rowWidth + 6 > widest) {
            gap = widest - (6 + labelWidth + rowWidth + 6);
            if (gap < PC_DEBUG_COLUMN_GAP_MIN_PIXELS) {
                gap = PC_DEBUG_COLUMN_GAP_MIN_PIXELS;
            }
        }
        _gPcDebugChoiceX = 6 + labelWidth + gap;
        if (_gPcDebugChoiceX + rowWidth + 6 > widest) {
            _gPcDebugChoiceX = widest - rowWidth - 6;
        }
        uiSetPanelContentSize(&object->panel, _gPcDebugChoiceX + rowWidth + 6, 0);
        object->panel.bounds.rect.x = -(object->panel.bounds.rect.w / 2);
        object->panel.bounds.rect.y = -(object->panel.bounds.rect.h / 2);
        _pcMenuFitTitle(&object->panel, _gPcMenuDebugTitle);
        owningTask->state = 1;
    }
    if (object->result != USER_INTERFACE_RESULT_CONFIRM) {
        object->result = USER_INTERFACE_RESULT_NONE;
    }
    uiDrawPanelLabel(&object->panel, (const char*)_gPcMenuDebugTitle);
    uiUpdateList(list, &object->panel);
    if (object->panel.control.word == USER_INTERFACE_PANEL_ACTIVE &&
        padCheckButtons(0, PAD_BUTTON_QUERY_PRESSED, gPadCancelButtonMask) != 0) {
        sndEvtRequestScriptStart(SOUND_MENU_CANCEL, 0, 0);
        object->result = USER_INTERFACE_RESULT_CONFIRM;
    }
}

static UiObjectDesc _gPcMenuDebugDesc = {
    USER_INTERFACE_PANEL_TITLE_STYLE, { -100, -60, 200, 120 }, 32, 0, TASK_BODY_NONE, TASK_PRIORITY_CONTROL, _pcMenuUpdateDebugTask, 0
};

/// Opens the debug panel, its place the player's own room.
static void _pcMenuOpenDebug(void)
{
    TaskNode*    previous;
    TaskSpawnArg none;
    s32          i;

    if (_gPcDebugRoom >= (s32)ARRAY_SIZE(_gPcDebugRooms)) {
        _gPcDebugRoom = 0;
    }
    for (i = 0; i < (s32)ARRAY_SIZE(_gPcDebugRooms); i++) {
        if (_gPcDebugRooms[i].stage == gGameSession->location.stage && _gPcDebugRooms[i].area == gGameSession->location.area) {
            if (i != _gPcDebugRoom) {
                _gPcDebugRoom     = i;
                _gPcDebugEntrance = 0;
            }
            break;
        }
    }
    _gPcDebugGo = 0;
    none.value  = 0;
    previous    = taskGetActiveList();
    taskSetActiveList(&_gPcMenuTasks);
    _gPcMenuDebugPanel = uiSpawnObject(&_gPcMenuDebugDesc, none, USER_INTERFACE_PANEL_ACTIVE, 1, NULL);
    taskSetActiveList(previous);
    _gPcMenuState = PC_MENU_DEBUG;
}

/// Go to room: the live save's location, then the session reload, as the
/// game's scripted transitions between rooms and stages do (actor_303600).
/// Story flags stay as they are, so a room may play an event out of order.
static void _pcMenuDebugWarp(void)
{
    const PcDebugRoom* room;
    GameLocationKey*   location;
    TaskSpawnArg       selector;
    TaskSpawnArg       mode;
    TaskSpawnArg       none;

    room                 = &_gPcDebugRooms[_gPcDebugRoom];
    location             = &gMcSaveData[MEMORY_CARD_SAVE_LIVE].state.location;
    location->stage      = room->stage;
    location->area       = room->area;
    location->warp       = _gPcDebugEntrance + 1;
    location->roomState  = 1;
    gDisplayState.spriteVariant = 1;
    selector.value = GAME_FLOW_RELOAD_TASK_SLOT;
    mode.value     = GAME_FLOW_RELOAD_CAPTURE_FRAME;
    none.value     = 0;
    taskSpawnOnDefaultList(GAME_FLOW_RELOAD_TASK_BANK, selector, mode, none);
}

static void _pcMenuOpenPause(void)
{
    u8* labels[6];
    s32 count;

    count                       = 0;
    _gPcMenuPauseRows[count]    = PC_MENU_THEN_RESUME;
    labels[count++]             = _gPcMenuResumeText;
    if (_pcMenuCanSave()) {
        _gPcMenuPauseRows[count] = PC_MENU_THEN_SAVE;
        labels[count++]          = _gPcMenuSaveText;
    }
    _gPcMenuPauseRows[count]    = PC_MENU_THEN_CATEGORIES;
    labels[count++]             = _gPcMenuOptionsText;
    if (_pcMenuCanDebug()) {
        _gPcMenuPauseRows[count] = PC_MENU_THEN_DEBUG;
        labels[count++]          = _gPcMenuDebugText;
    }
    if (_pcMenuCanQuitToTitle()) {
        _gPcMenuPauseRows[count] = PC_MENU_THEN_CONFIRM_TITLE;
        labels[count++]          = _gPcMenuToTitleText;
    }
    _gPcMenuPauseRows[count] = PC_MENU_THEN_CONFIRM_QUIT;
    labels[count++]          = _gPcMenuQuitGameText;
    _pcMenuOpenDialog(_gPcMenuPauseTitle, labels, count, 0);
    _gPcMenuState = PC_MENU_PAUSE;
}

static void _pcMenuOpenCategories(void)
{
    u8* labels[8];
    s32 count;
    s32 i;

    count = pc_options_categories();
    if (count > ARRAY_SIZE(labels)) {
        count = ARRAY_SIZE(labels);
    }
    for (i = 0; i < count; i++) {
        labels[i] = (u8*)pc_options_category(i, 0);
    }
    // Back from a category, on that category.
    _pcMenuOpenDialog(_gPcMenuOptionsTitle, labels, count,
                      _gPcMenuViews[PC_MENU_VIEW_PAUSE].category < count ? _gPcMenuViews[PC_MENU_VIEW_PAUSE].category : 0);
    _gPcMenuState = PC_MENU_CATEGORIES;
}

/// Draws a settings row: its label, then its choice between carets on the
/// selected row, "(restart)" beside a change that waits for one; Left and
/// Right change it. Laid out as the game's options rows are (options.c).
static void _pcMenuDrawSettingRow(UiList* list, UiObject* object)
{
    s32       row;
    s32       x;
    s32       y;
    u32       dim;
    const u8* choice;
    PcMenuSettingsView* view;

    // The view whose list this is.
    view = &_gPcMenuViews[list == &_gPcMenuViews[PC_MENU_VIEW_OPTIONS].list];
    row  = list->currentItemIndex;
    y   = list->rowTextY.signedValue;
    x   = object->panel.contentLeft.signedValue;
    dim = uiGetTextColor(object, USER_INTERFACE_TEXT_COLOR_DIMMED);
    textDrawUiLine(object, x + 6, y, pc_options_label(view->category, row), list->colorRgb, TEXT_DRAW_OUTLINED, TEXT_ALIGNMENT_LEFT);
    x += view->choiceX;
    choice = pc_options_choice(view->category, row, -1);
    if (list->selectedItemIndex == row) {
        textDrawUiLine(object, x, y, _gPcMenuCaretLeft, dim, TEXT_DRAW_OUTLINED, TEXT_ALIGNMENT_LEFT);
        textDrawUiLine(object, x + _gPcMenuCaretWidth + textMeasureLineWidth(choice) + PC_MENU_CARET_GAP_PIXELS, y,
                       _gPcMenuCaretRight, dim, TEXT_DRAW_OUTLINED, TEXT_ALIGNMENT_LEFT);
    }
    textDrawUiLine(object, x + _gPcMenuCaretWidth, y, choice, uiGetTextColor(object, USER_INTERFACE_TEXT_COLOR_NORMAL),
                   TEXT_DRAW_OUTLINED, TEXT_ALIGNMENT_LEFT);
    if (pc_options_needs_restart(view->category, row, 0)) {
        textDrawUiLine(object, x + 2 * _gPcMenuCaretWidth + view->rowChoiceWidth[row] + 4, y, pc_options_restart_text(),
                       dim, TEXT_DRAW_OUTLINED, TEXT_ALIGNMENT_LEFT);
    }
    if (list->rowInputEnabled == USER_INTERFACE_LIST_ROW_ACTIVE) {
        if (padCheckButtons(0, PAD_BUTTON_QUERY_PRESSED, PAD_BUTTON_RIGHT) != 0) {
            sndEvtRequestScriptStart(SOUND_MENU_CURSOR, 0, 0);
            pc_options_change(view->category, row, 1);
        } else if (padCheckButtons(0, PAD_BUTTON_QUERY_PRESSED, PAD_BUTTON_LEFT) != 0) {
            sndEvtRequestScriptStart(SOUND_MENU_CURSOR, 0, 0);
            pc_options_change(view->category, row, -1);
        }
    }
}

/// The settings panel's task: fitted to its rows on its first update, then
/// its title and rows each frame; Cancel publishes a confirm for the menu
/// to close it (as the game's options panel does).
static void _pcMenuUpdateSettingsTask(Task* owningTask)
{
    enum {
        PC_MENU_ROW_HEIGHT_PIXELS     = 16,
        PC_MENU_COLUMN_GAP_PIXELS     = 16,
        PC_MENU_COLUMN_GAP_MIN_PIXELS = 6,
        PC_MENU_SCREEN_MARGIN_PIXELS  = 8,
    };
    UiObject*  object;
    UiList*    list;
    const u8*  text;
    s32        rows;
    s32        row;
    s32        choice;
    s32        width;
    s32        labelWidth;
    s32        restartWidth;
    s32        rowWidth;
    s32        widest;
    s32        gap;
    PcMenuSettingsView* view;

    object = owningTask->spawnArg2.pointer;
    view   = &_gPcMenuViews[owningTask->spawnArg1.value == PC_MENU_VIEW_OPTIONS];
    list   = &view->list;
    if (owningTask->state == 0) {
        rows                            = pc_options_count(view->category);
        list->rowCallbacks              = _gPcMenuSettingRowCallbacks;
        list->itemCount                 = rows;
        list->visibleRowCount.unsignedValue = rows;
        list->wrapNavigation            = 0;
        list->rowHeight                 = PC_MENU_ROW_HEIGHT_PIXELS;
        list->selectedItemIndex         = 0;
        uiFitPanelToList(list, &object->panel);
        list->flags = USER_INTERFACE_LIST_SHARED_ROW_CALLBACK;
        // Columns: the widest label, then each row's widest choice, with
        // "(restart)" after it on a row that may need one.
        if (rows > ARRAY_SIZE(view->rowChoiceWidth)) {
            rows = ARRAY_SIZE(view->rowChoiceWidth);
        }
        restartWidth       = textMeasureLineWidth(pc_options_restart_text());
        _gPcMenuCaretWidth = textMeasureLineWidth(_gPcMenuCaretLeft) + PC_MENU_CARET_GAP_PIXELS;
        labelWidth         = 0;
        rowWidth           = 0;
        for (row = 0; row < rows; row++) {
            width = textMeasureLineWidth(pc_options_label(view->category, row));
            if (labelWidth < width) {
                labelWidth = width;
            }
            view->rowChoiceWidth[row] = 0;
            for (choice = 0; (text = pc_options_choice(view->category, row, choice)) != NULL; choice++) {
                width = textMeasureLineWidth(text);
                if (view->rowChoiceWidth[row] < width) {
                    view->rowChoiceWidth[row] = width;
                }
            }
            width = 2 * _gPcMenuCaretWidth + view->rowChoiceWidth[row];
            if (pc_options_needs_restart(view->category, row, 1)) {
                width += 4 + restartWidth;
            }
            if (rowWidth < width) {
                rowWidth = width;
            }
        }
        // No wider than the screen (with the game's eight-pixel margins):
        // a long translation first narrows the gap between the columns,
        // then lets the choices overlap the labels' ends.
        widest = DISPLAY_SCREEN_WIDTH - 2 * PC_MENU_SCREEN_MARGIN_PIXELS -
                 (object->panel.bounds.rect.w - (object->panel.contentRight.signedValue - object->panel.contentLeft.signedValue));
        gap = PC_MENU_COLUMN_GAP_PIXELS;
        if (6 + labelWidth + gap + rowWidth + 6 > widest) {
            gap = widest - (6 + labelWidth + rowWidth + 6);
            if (gap < PC_MENU_COLUMN_GAP_MIN_PIXELS) {
                gap = PC_MENU_COLUMN_GAP_MIN_PIXELS;
            }
        }
        view->choiceX = 6 + labelWidth + gap;
        if (view->choiceX + rowWidth + 6 > widest) {
            view->choiceX = widest - rowWidth - 6;
        }
        uiSetPanelContentSize(&object->panel, view->choiceX + rowWidth + 6, 0);
        object->panel.bounds.rect.x = -(object->panel.bounds.rect.w / 2);
        object->panel.bounds.rect.y = -(object->panel.bounds.rect.h / 2);
        _pcMenuFitTitle(&object->panel, pc_options_category(view->category, 1));
        owningTask->state = 1;
    }
    object->result = USER_INTERFACE_RESULT_NONE;
    uiDrawPanelLabel(&object->panel, (const char*)pc_options_category(view->category, 1));
    uiUpdateList(list, &object->panel);
    if (object->panel.control.word == USER_INTERFACE_PANEL_ACTIVE) {
        if (padCheckButtons(0, PAD_BUTTON_QUERY_PRESSED, gPadCancelButtonMask) != 0) {
            sndEvtRequestScriptStart(SOUND_MENU_CANCEL, 0, 0);
            object->result = USER_INTERFACE_RESULT_CONFIRM;
        } else if (padCheckButtons(0, PAD_BUTTON_QUERY_PRESSED, gPadMenuButtonMask) != 0) {
            // In the game's options: the Menu button leaves the whole menu,
            // as there (the pause menu ignores it).
            object->result = USER_INTERFACE_RESULT_CANCEL;
        }
    }
}

/// The settings panel's recipe: a titled panel, sized on its first update.
static UiObjectDesc _gPcMenuSettingsDesc = {
    USER_INTERFACE_PANEL_TITLE_STYLE, { -100, -60, 200, 120 }, 32, 0, TASK_BODY_NONE, TASK_PRIORITY_CONTROL, _pcMenuUpdateSettingsTask, 0
};

static void _pcMenuOpenSettings(void)
{
    TaskNode*    previous;
    TaskSpawnArg none;

    none.value = 0;
    previous   = taskGetActiveList();
    taskSetActiveList(&_gPcMenuTasks);
    _gPcMenuPanel = uiSpawnObject(&_gPcMenuSettingsDesc, none, USER_INTERFACE_PANEL_ACTIVE, 1, NULL);
    taskSetActiveList(previous);
    _gPcMenuState = PC_MENU_SETTINGS;
}

/* PE2PC's settings in the game's own Option list (options.c): a row opening
 * a panel of the categories, each opening the settings panel above, as the
 * list's Key Config row opens its chart. */
static u8 _gPcMenuSettingsText[]   = "PC Settings";
static u8 _gPcMenuSettingsTitle[]  = "PC SETTINGS";
static u8 _gPcMenuSettingsPrompt[] = "Open PE2PC's own settings.\nPress the } button to proceed.";
static void    _pcMenuDrawCategoryRow(UiList* list, UiObject* object);
static UiListRowCallback _gPcMenuCategoryRowCallbacks[] = { _pcMenuDrawCategoryRow };
static UiList  _gPcMenuCategoryList;

/// Draws a category row; Confirm opens its settings over the panel.
static void _pcMenuDrawCategoryRow(UiList* list, UiObject* object)
{
    TaskSpawnArg none;
    s32          row;

    row = list->currentItemIndex;
    textDrawUiLine(object, object->panel.contentLeft.signedValue + 6, list->rowTextY.signedValue, pc_options_category(row, 0),
                   list->colorRgb, TEXT_DRAW_OUTLINED, TEXT_ALIGNMENT_LEFT);
    if (list->rowInputEnabled == USER_INTERFACE_LIST_ROW_ACTIVE &&
        padCheckButtons(0, PAD_BUTTON_QUERY_PRESSED, gPadConfirmButtonMask) != 0) {
        sndEvtRequestScriptStart(SOUND_MENU_CONFIRM, 0, 0);
        _gPcMenuViews[PC_MENU_VIEW_OPTIONS].category = row;
        none.value = PC_MENU_VIEW_OPTIONS;
        uiSpawnObject(&_gPcMenuSettingsDesc, none, USER_INTERFACE_PANEL_ACTIVE, 1, object);
        object->panel.control.word = USER_INTERFACE_PANEL_INACTIVE;
    }
}

/// The categories panel's task, as the game's options panel's: Cancel
/// publishes a confirm (back to the Option list), Menu a cancel (out of the
/// menu); a settings panel closing writes the settings and comes back here.
static void _pcMenuUpdateCategoriesTask(Task* owningTask)
{
    enum { PC_MENU_CATEGORY_ROW_HEIGHT_PIXELS = 16 };
    UiObject* object;
    UiObject* child;
    UiList*   list;
    s32       count;
    s32       row;
    s32       width;
    s32       widest;

    object = owningTask->spawnArg2.pointer;
    list   = &_gPcMenuCategoryList;
    if (owningTask->state == 0) {
        count = pc_options_categories();
        list->rowCallbacks                  = _gPcMenuCategoryRowCallbacks;
        list->itemCount                     = count;
        list->visibleRowCount.unsignedValue = count;
        list->wrapNavigation                = 0;
        list->rowHeight                     = PC_MENU_CATEGORY_ROW_HEIGHT_PIXELS;
        list->selectedItemIndex             = _gPcMenuViews[PC_MENU_VIEW_OPTIONS].category < count ? _gPcMenuViews[PC_MENU_VIEW_OPTIONS].category : 0;
        uiFitPanelToList(list, &object->panel);
        list->flags = USER_INTERFACE_LIST_SHARED_ROW_CALLBACK;
        widest      = 0;
        for (row = 0; row < count; row++) {
            width = textMeasureLineWidth(pc_options_category(row, 0));
            if (widest < width) {
                widest = width;
            }
        }
        uiSetPanelContentSize(&object->panel, 6 + widest + 24, 0);
        object->panel.bounds.rect.x = -(object->panel.bounds.rect.w / 2);
        object->panel.bounds.rect.y = -(object->panel.bounds.rect.h / 2);
        _pcMenuFitTitle(&object->panel, _gPcMenuSettingsTitle);
        owningTask->state = 1;
    }
    object->result = USER_INTERFACE_RESULT_NONE;
    uiDrawPanelLabel(&object->panel, (const char*)_gPcMenuSettingsTitle);
    uiUpdateList(list, &object->panel);
    if (object->panel.control.word == USER_INTERFACE_PANEL_ACTIVE) {
        if (padCheckButtons(0, PAD_BUTTON_QUERY_PRESSED, gPadCancelButtonMask) != 0) {
            sndEvtRequestScriptStart(SOUND_MENU_CANCEL, 0, 0);
            object->result = USER_INTERFACE_RESULT_CONFIRM;
        } else if (padCheckButtons(0, PAD_BUTTON_QUERY_PRESSED, gPadMenuButtonMask) != 0) {
            object->result = USER_INTERFACE_RESULT_CANCEL;
        }
    }
    if (owningTask->firstChild != NULL) {
        child = owningTask->firstChild->spawnArg2.pointer;
        switch (child->result) {
            case USER_INTERFACE_RESULT_CONFIRM:
                pc_options_save();
                uiStartTreeClosing(child, child->owner);
                object->panel.control.word = USER_INTERFACE_PANEL_ACTIVE;
                break;
            case USER_INTERFACE_RESULT_CANCEL:
                pc_options_save();
                object->result = USER_INTERFACE_RESULT_CANCEL;
                break;
        }
    }
}

/// The categories panel's recipe: in front of the Option list (OT 56) and
/// behind the settings panel it opens (32).
static UiObjectDesc _gPcMenuCategoriesDesc = {
    USER_INTERFACE_PANEL_TITLE_STYLE, { -80, -60, 160, 120 }, 44, 0, TASK_BODY_NONE, TASK_PRIORITY_CONTROL, _pcMenuUpdateCategoriesTask, 0
};

/// The game's Option list's PE2PC row (options.c, native builds): drawn as
/// its Key Config row, Confirm opening the categories over the list, which
/// handles the child's results as it does the key chart's.
void pcOptionsUpdateSettingsRow(UiList* list, UiObject* object)
{
    TaskSpawnArg none;
    s32          controlMode;

    textDrawUiLine(object, object->panel.contentLeft.signedValue + 6, list->rowTextY.signedValue, _gPcMenuSettingsText,
                   list->colorRgb, TEXT_DRAW_OUTLINED, TEXT_ALIGNMENT_LEFT);
    controlMode = object->panel.control.word;
    if (((controlMode >> 16) == USER_INTERFACE_PANEL_ACTIVE || controlMode == USER_INTERFACE_PANEL_ACTIVE) &&
        list->selectedItemIndex == list->currentItemIndex) {
        uiSetPromptText(_gPcMenuSettingsPrompt, 0, 0);
    }
    if (list->rowInputEnabled == USER_INTERFACE_LIST_ROW_ACTIVE &&
        padCheckButtons(0, PAD_BUTTON_QUERY_PRESSED, gPadConfirmButtonMask) != 0) {
        sndEvtRequestScriptStart(SOUND_MENU_CONFIRM, 0, 0);
        none.value = 0;
        uiSpawnObject(&_gPcMenuCategoriesDesc, none, USER_INTERFACE_PANEL_ACTIVE, 1, object);
        object->panel.control.word = USER_INTERFACE_PANEL_INACTIVE;
    }
}

/// Opens the game's save (as a telephone's) over the held game.
static void _pcMenuOpenSave(void)
{
    TaskNode*    previous;
    TaskSpawnArg none;

    _gPcMenuSavePrepare();
    none.value = 0;
    previous   = taskGetActiveList();
    taskSetActiveList(&_gPcMenuTasks);
    _gPcMenuSave = uiSpawnObject(&gMcDialogDescriptors[MEMORY_CARD_DIALOG_SAVE], none, USER_INTERFACE_PANEL_ACTIVE, 1, NULL);
    taskSetActiveList(previous);
    if (_gPcMenuSave == NULL) {
        _gPcMenuSaveDone();
    }
    _gPcMenuState = PC_MENU_SAVE;
}

static void _pcMenuOpenConfirm(s32 state)
{
    u8* labels[2];

    // No first, as the safe choice.
    labels[0] = _gPcMenuNoText;
    labels[1] = _gPcMenuYesText;
    _pcMenuOpenDialog(state == PC_MENU_CONFIRM_TITLE ? _gPcMenuTitleTitle : _gPcMenuQuitTitle, labels, 2, 0);
    _gPcMenuState = state;
}

/// Whether the game's frames are ones the menu's can stand in for: the game
/// loop's own, at 320x240.
static s32 _pcMenuAvailable(void)
{
    return gDisplayState.displayOwner == DISPLAY_OWNER_GAME_LOOP && gDisplayState.height == DISPLAY_SCREEN_HEIGHT;
}

/// Whether the menu may open now: available, with nothing holding the
/// screen or streaming a scene.
static s32 _pcMenuCanOpen(s32 blocked)
{
    return !blocked && _pcMenuAvailable() && gCdCmdQueue.paceToSceneTiming == 0;
}

static void _pcMenuOpen(void)
{
    RECT area;

    // The last picture drawn, kept to be drawn again behind the menu.
    DrawSync(0);
    area   = gDisplayState.drawEnv[gDisplayState.otBuffer].clip;
    area.w = DISPLAY_SCREEN_WIDTH;
    area.h = DISPLAY_SCREEN_HEIGHT;
    StoreImage(&area, (u_long*)_gPcMenuScreen);
    DrawSync(0);

    _gPcMenuTasks.next = NULL;
    _gPcMenuTasks.prev = &_gPcMenuTasks;
    pcUiSaveState();
    sndEvtRequestScriptDuckAcquire();
    _gPcMenuViews[PC_MENU_VIEW_PAUSE].category = 0;
    _gPcMenuThen = PC_MENU_THEN_NOTHING;
    _pcMenuOpenPause();
}

/// After a dialog's answer: what to do once it has closed.
static void _pcMenuAnswer(s32 result)
{
    switch (_gPcMenuState) {
        case PC_MENU_PAUSE:
            _gPcMenuThen = result > 0 ? _gPcMenuPauseRows[result - 1] : PC_MENU_THEN_RESUME;
            break;
        case PC_MENU_CONFIRM_TITLE:
            _gPcMenuThen = result == 2 ? PC_MENU_THEN_TITLE : PC_MENU_THEN_PAUSE;
            break;
        case PC_MENU_CONFIRM_QUIT:
            _gPcMenuThen = result == 2 ? PC_MENU_THEN_QUIT : PC_MENU_THEN_PAUSE;
            break;
        case PC_MENU_CATEGORIES:
            if (result > 0) {
                _gPcMenuViews[PC_MENU_VIEW_PAUSE].category = result - 1;
                _gPcMenuThen     = PC_MENU_THEN_SETTINGS;
            } else {
                _gPcMenuThen = PC_MENU_THEN_PAUSE;
            }
            break;
    }
    _gPcMenuDialog = NULL;
}

/// Once the dialogs have closed: the next one, or back to the game.
static void _pcMenuAfterClosing(void)
{
    s32 then;

    then         = _gPcMenuThen;
    _gPcMenuThen = PC_MENU_THEN_NOTHING;
    switch (then) {
        case PC_MENU_THEN_PAUSE:
            _pcMenuOpenPause();
            return;
        case PC_MENU_THEN_CONFIRM_TITLE:
            _pcMenuOpenConfirm(PC_MENU_CONFIRM_TITLE);
            return;
        case PC_MENU_THEN_CATEGORIES:
            _pcMenuOpenCategories();
            return;
        case PC_MENU_THEN_SETTINGS:
            _pcMenuOpenSettings();
            if (_gPcMenuPanel != NULL) {
                return;
            }
            break;
        case PC_MENU_THEN_CONFIRM_QUIT:
            if (pc_menu_confirms_quit()) {
                _pcMenuOpenConfirm(PC_MENU_CONFIRM_QUIT);
                return;
            }
            pc_menu_quit();
            return;
        case PC_MENU_THEN_QUIT:
            pc_menu_quit();
            return;
        case PC_MENU_THEN_TITLE:
            _gPcMenuTitleRequested = 1;
            break;
        case PC_MENU_THEN_SAVE:
            _pcMenuOpenSave();
            if (_gPcMenuSave != NULL) {
                return;
            }
            break;
        case PC_MENU_THEN_DEBUG:
            _pcMenuOpenDebug();
            if (_gPcMenuDebugPanel != NULL) {
                return;
            }
            break;
    }
    // Back to the game: its dialogs as they were, its sound, and no button
    // the menu took reaching it.
    pcUiRestoreState();
    sndEvtRequestScriptDuckRelease();
    padStartInputBlock(0);
    _gPcMenuState = PC_MENU_CLOSED;
    if (then == PC_MENU_THEN_WARP) {
        _pcMenuDebugWarp();
    }
}

/// Draws the menu's frame: the saved picture, dimmed, under its ordering
/// table, into buffer `bufferIndex`, then shows it.
static void _pcMenuPresent(s32 bufferIndex)
{
    RECT area;
    s32  i;

    area   = gDisplayState.drawEnv[bufferIndex].clip;
    area.w = DISPLAY_SCREEN_WIDTH;
    area.h = DISPLAY_SCREEN_HEIGHT;
    PutDrawEnv(&gDisplayState.drawEnv[bufferIndex]);
    LoadImage(&area, (u_long*)_gPcMenuScreen);
    DrawSync(0);

    // Halved, as the game's own pause screen dims (gamemain.c).
    setDrawTPage(&_gPcMenuDimMode, true, true, getTPage(0, GPU_BLEND_AVERAGE, 0, 0));
    DrawPrim(&_gPcMenuDimMode);
    setTile(&_gPcMenuDimTile);
    setRGB0(&_gPcMenuDimTile, 0, 0, 0);
    setSemiTrans(&_gPcMenuDimTile, true);
    _gPcMenuDimTile.x0 = -DISPLAY_SCREEN_WIDTH / 2;
    _gPcMenuDimTile.y0 = -DISPLAY_SCREEN_HEIGHT / 2 - gDisplayState.vramYOffset;
    _gPcMenuDimTile.w  = DISPLAY_SCREEN_WIDTH;
    _gPcMenuDimTile.h  = DISPLAY_SCREEN_HEIGHT;
    DrawPrim(&_gPcMenuDimTile);

    DrawOTag((u_long*)gGpuFrameOrderingTables[bufferIndex].tag);
    DrawSync(0);
    VSync(0);
    // The display area showing what was just drawn.
    for (i = 0; i < 2; i++) {
        if (gDisplayState.dispEnv[i].disp.y == area.y && gDisplayState.dispEnv[i].disp.x == area.x) {
            PutDispEnv(&gDisplayState.dispEnv[i]);
        }
    }
}

s32 pcMenuFrame(s32 blocked)
{
    TaskNode* previous;
    s32       bufferIndex;

    // Whether the game draws its own frames, where the menu can open (a
    // moment's block aside): elsewhere the host asks to quit at once.
    pc_menu_available(_pcMenuAvailable());
    if (_gPcMenuState == PC_MENU_CLOSED) {
        if (!pc_menu_requested() || !_pcMenuCanOpen(blocked)) {
            return 0;
        }
        pc_menu_taken();
        _pcMenuOpen();
    } else if (pc_menu_requested()) {
        // Esc again: as the dialog's Cancel.
        pc_menu_taken();
        if (_gPcMenuSave != NULL) {
            // The save answers for itself (the card may be busy).
        } else if (_gPcMenuDialog != NULL) {
            uiStartTreeClosing(_gPcMenuDialog, NULL);
            _pcMenuAnswer(-1);
        } else if (_gPcMenuPanel != NULL) {
            _gPcMenuPanel->result = USER_INTERFACE_RESULT_CONFIRM;
        } else if (_gPcMenuDebugPanel != NULL) {
            _gPcMenuDebugPanel->result = USER_INTERFACE_RESULT_CONFIRM;
        }
    }

    // A frame of the menu's own: the buffers' other half, as the game's
    // frames alternate, and none of the game's clocks.
    bufferIndex              = gDisplayState.otBuffer ^ 1;
    gDisplayState.otBuffer   = bufferIndex;
    gDisplayState.drawBuffer = bufferIndex;
    gDisplayState.animFrame++;
    _gpuBeginOt(bufferIndex);
    gGpuSystemPrimCursor = gDisplayStaticPrimitiveArena + bufferIndex * (s32)(sizeof(gDisplayStaticPrimitiveArena) / 2);
    gGpuPrimCursor       = gGpuPrimitiveBufferBase + bufferIndex * (gGpuPrimitiveBufferBytes >> 1);

    previous = taskGetActiveList();
    taskExecList(&_gPcMenuTasks);
    taskSetActiveList(previous);

    if (_gPcMenuDialog != NULL && _gPcMenuRequest.result != 0) {
        _pcMenuAnswer(_gPcMenuRequest.result);
    }
    // The settings panel closed: its settings written, back to the categories.
    if (_gPcMenuPanel != NULL && _gPcMenuPanel->result == USER_INTERFACE_RESULT_CONFIRM) {
        pc_options_save();
        uiStartTreeClosing(_gPcMenuPanel, NULL);
        _gPcMenuPanel = NULL;
        _gPcMenuThen  = PC_MENU_THEN_CATEGORIES;
    }
    // The save answered (saved or not): back to the game.
    if (_gPcMenuSave != NULL && (_gPcMenuSave->result == USER_INTERFACE_RESULT_CONFIRM || _gPcMenuSave->result == USER_INTERFACE_RESULT_CANCEL)) {
        uiStartTreeClosing(_gPcMenuSave, NULL);
        _gPcMenuSave = NULL;
        _gPcMenuSaveDone();
        _gPcMenuThen = PC_MENU_THEN_RESUME;
    }
    // The debug panel closed: back to the pause dialog, or Go to room.
    if (_gPcMenuDebugPanel != NULL && _gPcMenuDebugPanel->result == USER_INTERFACE_RESULT_CONFIRM) {
        uiStartTreeClosing(_gPcMenuDebugPanel, NULL);
        _gPcMenuDebugPanel = NULL;
        _gPcMenuThen       = _gPcDebugGo ? PC_MENU_THEN_WARP : PC_MENU_THEN_PAUSE;
    }
    if (_gPcMenuDialog == NULL && _gPcMenuPanel == NULL && _gPcMenuSave == NULL && _gPcMenuDebugPanel == NULL &&
        _gPcMenuTasks.next == NULL) {
        _pcMenuAfterClosing();
    }

    cdCmdService();
    _pcMenuPresent(bufferIndex);
    return 1;
}

/// The menu's own texts (not the game's, so not on the disc), for the host's
/// extract to list for translators (mods' strings/ translate them as the
/// game's); `count` gets how many.
const u8* const* pcMenuTexts(s32* count)
{
    static const u8* const texts[] = {
        _gPcMenuPauseTitle, _gPcMenuResumeText, _gPcMenuSaveText, _gPcMenuOptionsText, gPcTextMovedToBox, gPcTextQuantity, gPcTextSetNumber, _gPcMenuToTitleText, _gPcMenuQuitGameText, _gPcMenuTitleTitle, _gPcMenuQuitTitle, _gPcMenuNoText, _gPcMenuYesText,
        _gPcMenuSettingsText, _gPcMenuSettingsTitle, _gPcMenuSettingsPrompt, _gPcMenuDebugText, _gPcMenuDebugTitle,
    };

    *count = ARRAY_SIZE(texts);
    return texts;
}

s32 pcMenuTakeTitleRequest(void)
{
    s32 requested;

    requested              = _gPcMenuTitleRequested;
    _gPcMenuTitleRequested = 0;
    return requested;
}

void pcMenuAllowSave(void (*prepare)(void), void (*done)(void))
{
    _gPcMenuSaveAllowedAt = gDisplayState.loopCount;
    _gPcMenuSavePrepare   = prepare;
    _gPcMenuSaveDone      = done;
}

#endif
