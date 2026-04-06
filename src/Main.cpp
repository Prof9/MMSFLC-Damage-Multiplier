#include <Enums_Internal.hpp>

#include <cassert>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <stdfloat>
#include <thread>
#include <utility>
#include <vector>

#include <Windows.h>

#include <reframework/API.hpp>

#include "Guid.hpp"
#include "MessageManager.hpp"
#include "MessageUtility.hpp"

#define CONFIG_DIR "reframework\\data\\DamageMultiplier"
#define CONFIG_FILENAME CONFIG_DIR "\\config.bin"
#define DEFAULT_DAMAGE_MULTIPLIER 1.0f

static const std::vector<std::pair<std::string, float>> NEW_DAMAGE_DOWN_VALUES = {
    {"Common_MsgGUI00030000_Difficult_Rate_0", 0.0f}, // 0%
    {"Common_MsgGUI00030000_Difficult_Rate_1", 0.2f}, // 20%
    {"Common_MsgGUI00030000_Difficult_Rate_2", 0.4f}, // 40%
    {"Common_MsgGUI00030000_Difficult_Rate_3", 0.6f}, // 60%
    {"Common_MsgGUI00030000_Difficult_Rate_4", 0.8f}, // 80%
    {"Common_MsgGUI00030000_Difficult_Rate_5", 1.0f}, // 100%
    {"Common_MsgGUI00030000_Difficult_Rate_6", 2.0f}, // 200%
    {"Common_MsgGUI00030000_Difficult_Rate_7", 3.0f}, // 300%
    {"Common_MsgGUI00030000_Difficult_Rate_8", 4.0f}, // 400%
    {"Common_MsgGUI00030000_Difficult_Rate_9", 5.0f}, // 500%
    //{"Common_MsgGUI00030000_Assist_11", 20000.0f}, // Maximum, causes HP underflow!
};

static float _damageMultiplier = DEFAULT_DAMAGE_MULTIPLIER;
static int _damageReductionIdx = -1;
static int _playerHPBefore = -1;
static reframework::API::ManagedObject *_battlePlayer = nullptr;
static reframework::API::ManagedObject *_menuTblOption = nullptr;

static std::size_t GetClosestDamageMultiplierIdx()
{
    std::size_t closest = 0;
    for (std::size_t i = 0; i < NEW_DAMAGE_DOWN_VALUES.size(); ++i)
    {
        float distCurrent = std::abs(_damageMultiplier - NEW_DAMAGE_DOWN_VALUES[i].second);
        float distClosest = std::abs(_damageMultiplier - NEW_DAMAGE_DOWN_VALUES[closest].second);
        if (distCurrent < distClosest)
        {
            closest = i;
        }
    }
    return closest;
}

extern "C" __declspec(dllexport) void reframework_plugin_required_version(REFrameworkPluginVersion *version)
{
    version->major = REFRAMEWORK_PLUGIN_VERSION_MAJOR;
    version->minor = REFRAMEWORK_PLUGIN_VERSION_MINOR;
    version->patch = REFRAMEWORK_PLUGIN_VERSION_PATCH;
}

extern "C" __declspec(dllexport) bool reframework_plugin_initialize(const REFrameworkPluginInitializeParam *param)
{
    auto &api = reframework::API::initialize(param);
    auto tdb = api->tdb();

    // Load damage multiplier from file
    std::ifstream configFile;
    configFile.open(CONFIG_FILENAME, std::fstream::in | std::fstream::binary);
    if (configFile.is_open())
    {
        configFile.read((char *)&_damageMultiplier, sizeof(_damageMultiplier));
        configFile.close();

        // Clamp damage multiplier to supported values
        _damageMultiplier = NEW_DAMAGE_DOWN_VALUES[GetClosestDamageMultiplierIdx()].second;
    }

    // Wait for Launcher to initialize
    reframework::API::ManagedObject *launcher = nullptr;
    for (int r = 0; r < 100; ++r)
    {
        launcher = api->get_managed_singleton("app.Launcher");
        if (launcher != nullptr)
        {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    assert(launcher != nullptr);

    // Get MessageManager
    void *messageManagerNativeObject = api->get_native_singleton("via.gui.MessageManager");
    assert(messageManagerNativeObject != nullptr);
    MessageManager messageManager(messageManagerNativeObject);

    // Replace options menu messages
    messageManager.replaceMessageByName(
        "Common_MsgGUI00030000_Difficult_1",
        {
            {via::Language::Japanese, u"被ダメージ軽減率"},
            {via::Language::English, u"Damage Multiplier"},
            {via::Language::TransitionalChinese, u"傷害下降率"},
            {via::Language::SimplelifiedChinese, u"受击伤害减轻率"},
        },
        via::Language::English);
    messageManager.replaceMessageByName(
        "Common_MsgGUI00030000_Difficult_Guid_1",
        {
            // The Japanese and Chinese strings don't mention a reduction so they can be left as-is
            // Included here anyway so that we can copy the changed description for all other languages
            {via::Language::Japanese, u"敵から受けるダメージを調整できます。"},
            {via::Language::English, u"Set the multiplier for damage taken from enemies."},
            {via::Language::TransitionalChinese, u"可調整敵人造成的傷害。"},
            {via::Language::SimplelifiedChinese, u"可以对所受到的敌方伤害进行调整。"},
        },
        via::Language::English);

    // Reroute damage rate getter to return our custom multiplier
    reframework::API::Method *getDamageDownRateMethod = tdb->find_method("app.Launcher", "GetDamageDownRate()");
    assert(getDamageDownRateMethod != nullptr);
    getDamageDownRateMethod->add_hook(
        nullptr,
        [](void **ret_val, auto...)
        {
            *(float *)ret_val = _damageMultiplier;
        },
        false);

    // Hook method which is called when exiting options tab for difficulty
    // Here we want to avoid writing the extended rate to the save file,
    // because doing so causes the game to freeze if it tries to load the
    // options menu without this mod active.
    // We also write the modified rate to file here, which is a little
    // earlier than the game does it, but w/e
    // (Game does it when you exit Settings altogether)
    reframework::API::Method *removeMenuMethod = tdb->find_method("app.cLauncherOptionDifficult", "removeMenu()");
    assert(removeMenuMethod != nullptr);
    removeMenuMethod->add_hook(
        [](int argc, void **argv, auto...)
        {
            _menuTblOption = (reframework::API::ManagedObject *)argv[1];

            return REFRAMEWORK_HOOK_CALL_ORIGINAL;
        },
        [](auto...)
        {
            reframework::InvokeRet x;
            auto &api = reframework::API::get();
            auto tdb = api->tdb();

            assert(_menuTblOption != nullptr);
            assert(_damageReductionIdx != -1);

            reframework::API::ManagedObject *launcher = api->get_managed_singleton("app.Launcher");
            assert(launcher != nullptr);

            // set launcher.damageDownRate to default
            std::int32_t *damageDownRate_ptr = launcher->get_field<std::int32_t>("damageDownRate");
            assert(damageDownRate_ptr != nullptr);
            *damageDownRate_ptr = 0;

            // get launcherOptionDifficult._CursolIndex
            reframework::API::ManagedObject **cursorIndex_ptr = _menuTblOption->get_field<reframework::API::ManagedObject *>("_CursolIndex");
            assert(cursorIndex_ptr != nullptr);
            reframework::API::ManagedObject *cursorIndex = *cursorIndex_ptr;
            assert(cursorIndex != nullptr);

            // get _CursolIndex[damageReductionIdx]
            x = cursorIndex->invoke(
                "get_Item",
                {
                    (void *)(intptr_t)_damageReductionIdx,
                });
            int damageMultiplierIdx = x.dword;

            // set damage multiplier
            _damageMultiplier = NEW_DAMAGE_DOWN_VALUES[damageMultiplierIdx].second;

            // write damage multiplier to file
            std::filesystem::create_directories(CONFIG_DIR);
            std::ofstream configFile;
            configFile.open(CONFIG_FILENAME, std::fstream::out | std::fstream::binary);
            if (configFile.is_open())
            {
                configFile.write((char *)&_damageMultiplier, sizeof(_damageMultiplier));
                configFile.close();
            }
            else
            {
                api->log_error("Failed to save damage multiplier to file!");
            }
        },
        false);

    // Hook method which is called when Settings menu is initialized
    // Here we modify the layout of the damage reduction setting to add additional rates
    reframework::API::Method *startMethod = tdb->find_method("app.GUILauncherOption", "start()");
    assert(startMethod != nullptr);
    startMethod->add_hook(
        [](auto...)
        {
            reframework::InvokeRet x;
            auto &api = reframework::API::get();
            auto tdb = api->tdb();

            reframework::API::ManagedObject *launcher = api->get_managed_singleton("app.Launcher");
            assert(launcher != nullptr);

            // get launcher.damageDownValues
            reframework::API::ManagedObject **damageDownValues_ptr = launcher->get_field<reframework::API::ManagedObject *>("damageDownValues");
            assert(damageDownValues_ptr != nullptr);
            reframework::API::ManagedObject *damageDownValues = *damageDownValues_ptr;
            assert(damageDownValues != nullptr);

            // get launcher.guiBehaviors
            reframework::API::ManagedObject **guiBehaviors_ptr = launcher->get_field<reframework::API::ManagedObject *>("guiBehaviors");
            assert(guiBehaviors_ptr != nullptr);
            reframework::API::ManagedObject *guiBehaviors = *guiBehaviors_ptr;
            assert(guiBehaviors != nullptr);

            // get guiBehaviors[app::Launcher::LauncherGUIId::Option]
            x = guiBehaviors->invoke(
                "get_Item",
                {
                    (void *)(intptr_t)std::to_underlying(app::Launcher::LauncherGUIId::Option),
                });
            reframework::API::ManagedObject *launcherOption = (reframework::API::ManagedObject *)x.ptr;
            assert(launcherOption != nullptr);

            // get launcherOption._MenuTbl
            reframework::API::ManagedObject **menuTbl_ptr = launcherOption->get_field<reframework::API::ManagedObject *>("_MenuTbl");
            assert(menuTbl_ptr != nullptr);
            reframework::API::ManagedObject *menuTbl = *menuTbl_ptr;
            assert(menuTbl != nullptr);

            // get menuTbl[app::Launcher::LauncherGUIId::Option]
            x = menuTbl->invoke(
                "get_Item",
                {
                    (void *)(intptr_t)std::to_underlying(app::GUILauncherOption::MENU_TYPE::DEFFICULT),
                });
            _menuTblOption = (reframework::API::ManagedObject *)x.ptr;
            assert(_menuTblOption != nullptr);

            // get menuTblOption.NameList
            reframework::API::ManagedObject **nameList_ptr = _menuTblOption->get_field<reframework::API::ManagedObject *>("NameList");
            assert(nameList_ptr != nullptr);
            reframework::API::ManagedObject *nameList = *nameList_ptr;
            assert(nameList != nullptr);

            // get menuTblOption.MenuNum
            std::int32_t *menuNum_ptr = _menuTblOption->get_field<std::int32_t>("MenuNum");
            assert(menuNum_ptr != nullptr);
            std::int32_t menuNum = *menuNum_ptr;

            // get GUID for Damage Reduction string
            Guid *guidDamageReduction = MessageUtility::getMessageGuidByName("Common_MsgGUI00030000_Difficult_1");
            assert(guidDamageReduction != nullptr);

            // find the Damage Reduction option in the menu
            for (int i = 0; i < menuNum; ++i)
            {
                // get nameList[i]
                x = nameList->invoke(
                    "get_Item",
                    {
                        (void *)(intptr_t)i,
                    });
                Guid *nameListI = (Guid *)&x;

                if (*nameListI == *guidDamageReduction)
                {
                    _damageReductionIdx = i;
                    break;
                }
            }
            assert(_damageReductionIdx != -1);

            // get System.Guid type
            reframework::API::TypeDefinition *guidType = tdb->find_type("System.Guid");
            assert(guidType != nullptr);

            // create new System.Guid[NEW_DAMAGE_DOWN_VALUES.size()]
            reframework::API::ManagedObject *newDamageDownName = api->create_managed_array(guidType, NEW_DAMAGE_DOWN_VALUES.size());
            assert(newDamageDownName != nullptr);
            newDamageDownName->add_ref();

            // Fill new array
            for (int i = 0; i < NEW_DAMAGE_DOWN_VALUES.size(); ++i)
            {
                Guid *nameGuid = MessageUtility::getMessageGuidByName(NEW_DAMAGE_DOWN_VALUES[i].first);
                assert(nameGuid != nullptr);

                // set newDamageDownName[i]
                newDamageDownName->invoke(
                    "set_Item",
                    {
                        (void *)(intptr_t)i,
                        (void *)nameGuid,
                    });
            }

            // get menuTblOption.DamageDownName
            reframework::API::ManagedObject **damageDownName_ptr = _menuTblOption->get_field<reframework::API::ManagedObject *>("DamageDownName");
            assert(damageDownName_ptr != nullptr);
            reframework::API::ManagedObject *damageDownName = *damageDownName_ptr;
            assert(damageDownName != nullptr);

            // set menuTblOption.DamageDownName
            damageDownName->release();
            *damageDownName_ptr = newDamageDownName;

            // get menuTblOption.CursolMax
            reframework::API::ManagedObject **cursorMax_ptr = _menuTblOption->get_field<reframework::API::ManagedObject *>("CursolMax");
            assert(cursorMax_ptr != nullptr);
            reframework::API::ManagedObject *cursorMax = *cursorMax_ptr;
            assert(cursorMax != nullptr);

            // set CursolMax[damageReductionIdx]
            cursorMax->invoke(
                "set_Item",
                {
                    (void *)(intptr_t)_damageReductionIdx,
                    (void *)NEW_DAMAGE_DOWN_VALUES.size(),
                });

            return REFRAMEWORK_HOOK_CALL_ORIGINAL;
        },
        nullptr,
        false);

    // Hook method which is called when the game populates the Options menu
    // with the actual saved settings from the player's save file
    // Here we overwrite the selected index for damage reduction with our custom rate
    reframework::API::Method *getSaveParamMethod = tdb->find_method("app.cLauncherOptionDifficult", "getSaveParam()");
    assert(getSaveParamMethod != nullptr);
    getSaveParamMethod->add_hook(
        nullptr,
        [](auto...)
        {
            reframework::InvokeRet x;

            // get launcherOptionDifficult._CursolIndex
            reframework::API::ManagedObject **cursorIndex_ptr = _menuTblOption->get_field<reframework::API::ManagedObject *>("_CursolIndex");
            assert(cursorIndex_ptr != nullptr);
            reframework::API::ManagedObject *cursorIndex = *cursorIndex_ptr;
            assert(cursorIndex != nullptr);

            // set _CursolIndex[damageReductionIdx]
            x = cursorIndex->invoke(
                "set_Item",
                {
                    (void *)(intptr_t)_damageReductionIdx,
                    (void *)GetClosestDamageMultiplierIdx(),
                });
        },
        false);

    // Hook "Reset Settings" option used inside Difficulty tab
    // Here we reset our custom damage multiplier to 100%
    // This is needed because our 100% multiplier is in a different list index
    // than the original 0% reduction
    reframework::API::Method *resetSettingMethod = tdb->find_method("app.cLauncherOptionDifficult", "resetSetting()");
    assert(resetSettingMethod != nullptr);
    resetSettingMethod->add_hook(
        [](auto...)
        {
            _damageMultiplier = DEFAULT_DAMAGE_MULTIPLIER;

            return REFRAMEWORK_HOOK_CALL_ORIGINAL;
        },
        [](auto...)
        {
            reframework::InvokeRet x;

            // get launcherOptionDifficult._CursolIndex
            reframework::API::ManagedObject **cursorIndex_ptr = _menuTblOption->get_field<reframework::API::ManagedObject *>("_CursolIndex");
            assert(cursorIndex_ptr != nullptr);
            reframework::API::ManagedObject *cursorIndex = *cursorIndex_ptr;
            assert(cursorIndex != nullptr);

            // set _CursolIndex[damageReductionIdx]
            x = cursorIndex->invoke(
                "set_Item",
                {
                    (void *)(intptr_t)_damageReductionIdx,
                    (void *)GetClosestDamageMultiplierIdx(),
                });
        },
        false);

    return true;
}
