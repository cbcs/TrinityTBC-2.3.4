
#include "Config.h"
#include "DatabaseEnv.h"
#include "DBCStores.h"
#include "ObjectMgr.h"
#include "Spell.h"
#include "Config.h"
#include "ScriptMgr.h"
#include "GridNotifiers.h"
#include "Player.h"
#include "GossipDef.h"
#include "SpellScript.h"
#include "ScriptReloadMgr.h"

#ifndef _TRINITY_SCRIPT_CONFIG
# define _TRINITY_SCRIPT_CONFIG  "worldserver.conf"
#endif // _TRINITY_SCRIPT_CONFIG

// String text additional data, used in TextMap
struct StringTextData
{
    uint32 SoundId;
    uint8  Type;
    uint32 Language;
    uint32 Emote;
};

// Trait which indicates whether this script type
// must be assigned in the database.
template<typename>
struct is_script_database_bound
	: std::false_type { };

template<>
struct is_script_database_bound<SpellScriptLoader>
	: std::true_type { };

template<>
struct is_script_database_bound<InstanceMapScript>
	: std::true_type { };

template<>
struct is_script_database_bound<ItemScript>
	: std::true_type { };

template<>
struct is_script_database_bound<CreatureScript>
	: std::true_type { };

template<>
struct is_script_database_bound<GameObjectScript>
	: std::true_type { };

#ifdef LICH_KING
template<>
struct is_script_database_bound<VehicleScript>
	: std::true_type { };
#endif

template<>
struct is_script_database_bound<AreaTriggerScript>
	: std::true_type { };

/*
template<>
struct is_script_database_bound<BattlegroundScript>
	: std::true_type { };

template<>
struct is_script_database_bound<OutdoorPvPScript>
	: std::true_type { };

template<>
struct is_script_database_bound<WeatherScript>
	: std::true_type { };

template<>
struct is_script_database_bound<ConditionScript>
	: std::true_type { };

template<>
struct is_script_database_bound<TransportScript>
	: std::true_type { };

template<>
struct is_script_database_bound<AchievementCriteriaScript>
	: std::true_type { };
	*/

enum Spells
{
	SPELL_HOTSWAP_VISUAL_SPELL_EFFECT = 40162 // 59084
};

class ScriptRegistryInterface
{
public:
    ScriptRegistryInterface() { }
    virtual ~ScriptRegistryInterface() { }

    ScriptRegistryInterface(ScriptRegistryInterface const&) = delete;
    ScriptRegistryInterface(ScriptRegistryInterface&&) = delete;

    ScriptRegistryInterface& operator= (ScriptRegistryInterface const&) = delete;
    ScriptRegistryInterface& operator= (ScriptRegistryInterface&&) = delete;

    /// Removes all scripts associated with the given script context.
    /// Requires ScriptRegistryBase::SwapContext to be called after all transfers have finished.
    virtual void ReleaseContext(std::string const& context) = 0;

    /// Injects and updates the changed script objects.
    virtual void SwapContext(bool initialize) = 0;

    /// Removes the scripts used by this registry from the given container.
    /// Used to find unused script names.
    virtual void RemoveUsedScriptsFromContainer(std::unordered_set<std::string>& scripts) = 0;

    /// Unloads the script registry.
    virtual void Unload() = 0;
};

template<class>
class ScriptRegistry;

class ScriptRegistryCompositum
    : public ScriptRegistryInterface
{
    ScriptRegistryCompositum() { }

    template<class>
    friend class ScriptRegistry;

    /// Type erasure wrapper for objects
    class DeleteableObjectBase
    {
    public:
        DeleteableObjectBase() { }
        virtual ~DeleteableObjectBase() { }

        DeleteableObjectBase(DeleteableObjectBase const&) = delete;
        DeleteableObjectBase& operator= (DeleteableObjectBase const&) = delete;
    };

    template<typename T>
    class DeleteableObject
        : public DeleteableObjectBase
    {
    public:
        DeleteableObject(T&& object)
            : _object(std::forward<T>(object)) { }

    private:
        T _object;
    };

public:
    void SetScriptNameInContext(std::string const& scriptname, std::string const& context)
    {
        ASSERT(_scriptnames_to_context.find(scriptname) == _scriptnames_to_context.end(),
            "Scriptname was assigned to this context already!");
        _scriptnames_to_context.insert(std::make_pair(scriptname, context));
    }

    std::string const& GetScriptContextOfScriptName(std::string const& scriptname) const
    {
        auto itr = _scriptnames_to_context.find(scriptname);
        ASSERT(itr != _scriptnames_to_context.end() &&
            "Given scriptname doesn't exist!");
        return itr->second;
    }

    void ReleaseContext(std::string const& context) final override
    {
        for (auto const registry : _registries)
            registry->ReleaseContext(context);

        // Clear the script names in context after calling the release hooks
        // since it's possible that new references to a shared library
        // are acquired when releasing.
        for (auto itr = _scriptnames_to_context.begin();
            itr != _scriptnames_to_context.end();)
            if (itr->second == context)
                itr = _scriptnames_to_context.erase(itr);
            else
                ++itr;
    }

    void SwapContext(bool initialize) final override
    {
        for (auto const registry : _registries)
            registry->SwapContext(initialize);

        DoDelayedDelete();
    }

    void RemoveUsedScriptsFromContainer(std::unordered_set<std::string>& scripts) final override
    {
        for (auto const registry : _registries)
            registry->RemoveUsedScriptsFromContainer(scripts);
    }

    void Unload() final override
    {
        for (auto const registry : _registries)
            registry->Unload();
    }

    template<typename T>
    void QueueForDelayedDelete(T&& any)
    {
        _delayed_delete_queue.push_back(
            Trinity::make_unique<
            DeleteableObject<typename std::decay<T>::type>
            >(std::forward<T>(any))
        );
    }

    static ScriptRegistryCompositum* Instance()
    {
        static ScriptRegistryCompositum instance;
        return &instance;
    }

private:
    void Register(ScriptRegistryInterface* registry)
    {
        _registries.insert(registry);
    }

    void DoDelayedDelete()
    {
        _delayed_delete_queue.clear();
    }

    std::unordered_set<ScriptRegistryInterface*> _registries;

    std::vector<std::unique_ptr<DeleteableObjectBase>> _delayed_delete_queue;

    std::unordered_map<
        std::string /*script name*/,
        std::string /*context*/
    > _scriptnames_to_context;
};

#define sScriptRegistryCompositum ScriptRegistryCompositum::Instance()

template<typename /*ScriptType*/, bool /*IsDatabaseBound*/>
class SpecializedScriptRegistry;

// This is the global static registry of scripts.
template<class ScriptType>
class ScriptRegistry final
    : public SpecializedScriptRegistry<
    ScriptType, is_script_database_bound<ScriptType>::value>
{
    ScriptRegistry()
    {
        sScriptRegistryCompositum->Register(this);
    }

public:
    static ScriptRegistry* Instance()
    {
        static ScriptRegistry instance;
        return &instance;
    }

    void LogDuplicatedScriptPointerError(ScriptType const* first, ScriptType const* second)
    {
        // See if the script is using the same memory as another script. If this happens, it means that
        // someone forgot to allocate new memory for a script.
        TC_LOG_ERROR("scripts", "Script '%s' has same memory pointer as '%s'.",
            first->GetName().c_str(), second->GetName().c_str());
    }
};

class ScriptRegistrySwapHookBase
{
public:
    ScriptRegistrySwapHookBase() { }
    virtual ~ScriptRegistrySwapHookBase() { }

    ScriptRegistrySwapHookBase(ScriptRegistrySwapHookBase const&) = delete;
    ScriptRegistrySwapHookBase(ScriptRegistrySwapHookBase&&) = delete;

    ScriptRegistrySwapHookBase& operator= (ScriptRegistrySwapHookBase const&) = delete;
    ScriptRegistrySwapHookBase& operator= (ScriptRegistrySwapHookBase&&) = delete;

    /// Called before the actual context release happens
    virtual void BeforeReleaseContext(std::string const& /*context*/) { }

    /// Called before SwapContext
    virtual void BeforeSwapContext(bool /*initialize*/) { }

    /// Called before Unload
    virtual void BeforeUnload() { }
};

template<typename ScriptType, typename Base>
class ScriptRegistrySwapHooks
    : public ScriptRegistrySwapHookBase
{
};

/// This hook is responsible for swapping OutdoorPvP's
template<typename Base>
class UnsupportedScriptRegistrySwapHooks
    : public ScriptRegistrySwapHookBase
{
public:
    void BeforeReleaseContext(std::string const& context) final override
    {
        auto const bounds = static_cast<Base*>(this)->_ids_of_contexts.equal_range(context);
        ASSERT(bounds.first == bounds.second);
    }
};

/// This hook is responsible for swapping Creature and GameObject AI's
template<typename ObjectType, typename ScriptType, typename Base>
class CreatureGameObjectScriptRegistrySwapHooks
    : public ScriptRegistrySwapHookBase
{
    template<typename W>
    class AIFunctionMapWorker
    {
    public:
        template<typename T>
        AIFunctionMapWorker(T&& worker)
            : _worker(std::forward<T>(worker)) { }

        void Visit(std::unordered_map<ObjectGuid, ObjectType*>& objects)
        {
            _worker(objects);
        }

        template<typename O>
        void Visit(std::unordered_map<ObjectGuid, O*>&) { }

    private:
        W _worker;
    };

    class AsyncCastHotswapEffectEvent : public BasicEvent
    {
    public:
        explicit AsyncCastHotswapEffectEvent(Unit* owner) : owner_(owner) { }

        bool Execute(uint64 /*e_time*/, uint32 /*p_time*/) override
        {
            owner_->CastSpell(owner_, SPELL_HOTSWAP_VISUAL_SPELL_EFFECT, true);
            return true;
        }

    private:
        Unit* owner_;
    };

    // Hook which is called before a creature is swapped
    static void UnloadResetScript(Creature* creature)
    {
        // Remove deletable events only,
        // otherwise it causes crashes with non-deletable spell events.
        creature->m_Events.KillAllEvents(false);

        if (creature->IsCharmed())
            creature->RemoveCharmedBy(nullptr);

        ASSERT(!creature->IsCharmed(),
            "There is a disabled AI which is still loaded.");

        creature->AI()->EnterEvadeMode();
    }

    static void UnloadDestroyScript(Creature* creature)
    {
        bool const destroyed = creature->AIM_Destroy();
        ASSERT(destroyed,
            "Destroying the AI should never fail here!");
        (void)destroyed;

        ASSERT(!creature->AI(),
            "The AI should be null here!");
    }

    // Hook which is called before a gameobject is swapped
    static void UnloadResetScript(GameObject* gameobject)
    {
        gameobject->AI()->Reset();
    }

    static void UnloadDestroyScript(GameObject* gameobject)
    {
        gameobject->AIM_Destroy();

        ASSERT(!gameobject->AI(),
            "The AI should be null here!");
    }

    // Hook which is called after a creature was swapped
    static void LoadInitializeScript(Creature* creature)
    {
        ASSERT(!creature->AI(),
            "The AI should be null here!");

        if (creature->IsAlive())
            creature->ClearUnitState(UNIT_STATE_EVADE);

        bool const created = creature->AIM_Initialize();
        ASSERT(created,
            "Creating the AI should never fail here!");
        (void)created;
    }

    static void LoadResetScript(Creature* creature)
    {
        if (!creature->IsAlive())
            return;

        creature->AI()->EnterEvadeMode();

        // Cast a dummy visual spell asynchronously here to signal
        // that the AI was hot swapped
        creature->m_Events.AddEvent(new AsyncCastHotswapEffectEvent(creature),
            creature->m_Events.CalculateTime(0));
    }

    // Hook which is called after a gameobject was swapped
    static void LoadInitializeScript(GameObject* gameobject)
    {
        ASSERT(!gameobject->AI(),
            "The AI should be null here!");

        gameobject->AIM_Initialize();
    }

    static void LoadResetScript(GameObject* gameobject)
    {
        gameobject->AI()->Reset();
    }

    static Creature* GetEntityFromMap(std::common_type<Creature>, Map* map, ObjectGuid const& guid)
    {
        return map->GetCreature(guid);
    }

    static GameObject* GetEntityFromMap(std::common_type<GameObject>, Map* map, ObjectGuid const& guid)
    {
        return map->GetGameObject(guid);
    }

    template<typename T>
    static void VisitObjectsToSwapOnMap(Map* map, std::unordered_set<uint32> const& idsToRemove, T visitor)
    {
        auto evaluator = [&](std::unordered_map<ObjectGuid, ObjectType*>& objects)
        {
            for (auto object : objects)
            {
                // When the script Id of the script isn't removed in this
                // context change, do nothing.
                if (idsToRemove.find(object.second->GetScriptId()) != idsToRemove.end())
                    visitor(object.second);
            }
        };

        AIFunctionMapWorker<typename std::decay<decltype(evaluator)>::type> worker(std::move(evaluator));
        TypeContainerVisitor<decltype(worker), MapStoredObjectTypesContainer> containerVisitor(worker);

        containerVisitor.Visit(map->GetObjectsStore());
    }

    static void DestroyScriptIdsFromSet(std::unordered_set<uint32> const& idsToRemove)
    {
        // First reset all swapped scripts safe by guid
        // Skip creatures and gameobjects with an empty guid
        // (that were not added to the world as of now)
        sMapMgr->DoForAllMaps([&](Map* map)
        {
            std::vector<ObjectGuid> guidsToReset;

            VisitObjectsToSwapOnMap(map, idsToRemove, [&](ObjectType* object)
            {
                if (object->AI() && !object->GetGUID().IsEmpty())
                    guidsToReset.push_back(object->GetGUID());
            });

            for (ObjectGuid const& guid : guidsToReset)
            {
                if (auto entity = GetEntityFromMap(std::common_type<ObjectType>{}, map, guid))
                    UnloadResetScript(entity);
            }

            VisitObjectsToSwapOnMap(map, idsToRemove, [&](ObjectType* object)
            {
                // Destroy the scripts instantly
                UnloadDestroyScript(object);
            });
        });
    }

    static void InitializeScriptIdsFromSet(std::unordered_set<uint32> const& idsToRemove)
    {
        sMapMgr->DoForAllMaps([&](Map* map)
        {
            std::vector<ObjectGuid> guidsToReset;

            VisitObjectsToSwapOnMap(map, idsToRemove, [&](ObjectType* object)
            {
                if (!object->AI() && !object->GetGUID().IsEmpty())
                {
                    // Initialize the script
                    LoadInitializeScript(object);
                    guidsToReset.push_back(object->GetGUID());
                }
            });

            for (ObjectGuid const& guid : guidsToReset)
            {
                // Reset the script
                if (auto entity = GetEntityFromMap(std::common_type<ObjectType>{}, map, guid))
                {
                    if (!entity->AI())
                        LoadInitializeScript(entity);

                    LoadResetScript(entity);
                }
            }
        });
    }

public:
    void BeforeReleaseContext(std::string const& context) final override
    {
        auto idsToRemove = static_cast<Base*>(this)->GetScriptIDsToRemove(context);
        DestroyScriptIdsFromSet(idsToRemove);

        // Add the new ids which are removed to the global ids to remove set
        ids_removed_.insert(idsToRemove.begin(), idsToRemove.end());
    }

    void BeforeSwapContext(bool initialize) override
    {
        // Never swap creature or gameobject scripts when initializing
        if (initialize)
            return;

        // Add the recently added scripts to the deleted scripts to replace
        // default AI's with recently added core scripts.
        ids_removed_.insert(static_cast<Base*>(this)->GetRecentlyAddedScriptIDs().begin(),
            static_cast<Base*>(this)->GetRecentlyAddedScriptIDs().end());

        DestroyScriptIdsFromSet(ids_removed_);
        InitializeScriptIdsFromSet(ids_removed_);

        ids_removed_.clear();
    }

    void BeforeUnload() final override
    {
        ASSERT(ids_removed_.empty());
    }

private:
    std::unordered_set<uint32> ids_removed_;
};

// This hook is responsible for swapping CreatureAI's
template<typename Base>
class ScriptRegistrySwapHooks<CreatureScript, Base>
    : public CreatureGameObjectScriptRegistrySwapHooks<
    Creature, CreatureScript, Base
    > { };

// This hook is responsible for swapping GameObjectAI's
template<typename Base>
class ScriptRegistrySwapHooks<GameObjectScript, Base>
    : public CreatureGameObjectScriptRegistrySwapHooks<
    GameObject, GameObjectScript, Base
    > { };

/// This hook is responsible for swapping BattlegroundScript's
/*
template<typename Base>
class ScriptRegistrySwapHooks<BattlegroundScript, Base>
    : public UnsupportedScriptRegistrySwapHooks<Base> { };
*/

/// This hook is responsible for swapping OutdoorPvP's
/*
template<typename Base>
class ScriptRegistrySwapHooks<OutdoorPvPScript, Base>
    : public ScriptRegistrySwapHookBase
{
public:
    ScriptRegistrySwapHooks() : swapped(false) { }

    void BeforeReleaseContext(std::string const& context) final override
    {
        auto const bounds = static_cast<Base*>(this)->_ids_of_contexts.equal_range(context);

        if ((!swapped) && (bounds.first != bounds.second))
        {
            swapped = true;
            sOutdoorPvPMgr->Die();
        }
    }

    void BeforeSwapContext(bool initialize) override
    {
        // Never swap outdoor pvp scripts when initializing
        if ((!initialize) && swapped)
        {
            sOutdoorPvPMgr->InitOutdoorPvP();
            swapped = false;
        }
    }

    void BeforeUnload() final override
    {
        ASSERT(!swapped);
    }

private:
    bool swapped;
};
*/

/// This hook is responsible for swapping InstanceMapScript's
template<typename Base>
class ScriptRegistrySwapHooks<InstanceMapScript, Base>
    : public ScriptRegistrySwapHookBase
{
public:
    ScriptRegistrySwapHooks()  : swapped(false) { }

    void BeforeReleaseContext(std::string const& context) final override
    {
        auto const bounds = static_cast<Base*>(this)->_ids_of_contexts.equal_range(context);
        if (bounds.first != bounds.second)
            swapped = true;
    }

    void BeforeSwapContext(bool /*initialize*/) override
    {
        swapped = false;
    }

    void BeforeUnload() final override
    {
        ASSERT(!swapped);
    }

private:
    bool swapped;
};

/// This hook is responsible for swapping SpellScriptLoader's
template<typename Base>
class ScriptRegistrySwapHooks<SpellScriptLoader, Base>
    : public ScriptRegistrySwapHookBase
{
public:
    ScriptRegistrySwapHooks() : swapped(false) { }

    void BeforeReleaseContext(std::string const& context) final override
    {
        auto const bounds = static_cast<Base*>(this)->_ids_of_contexts.equal_range(context);

        if (bounds.first != bounds.second)
            swapped = true;
    }

    void BeforeSwapContext(bool /*initialize*/) override
    {
        if (swapped)
        {
            sObjectMgr->ValidateSpellScripts();
            swapped = false;
        }
    }

    void BeforeUnload() final override
    {
        ASSERT(!swapped);
    }

private:
    bool swapped;
};

// Database bound script registry
template<typename ScriptType>
class SpecializedScriptRegistry<ScriptType, true>
    : public ScriptRegistryInterface,
    public ScriptRegistrySwapHooks<ScriptType, ScriptRegistry<ScriptType>>
{
    template<typename>
    friend class UnsupportedScriptRegistrySwapHooks;

    template<typename, typename>
    friend class ScriptRegistrySwapHooks;

    template<typename, typename, typename>
    friend class CreatureGameObjectScriptRegistrySwapHooks;

public:
    SpecializedScriptRegistry() { }

    typedef std::unordered_map<
        uint32 /*script id*/,
        std::unique_ptr<ScriptType>
    > ScriptStoreType;

    typedef typename ScriptStoreType::iterator ScriptStoreIteratorType;

    void ReleaseContext(std::string const& context) final override
    {
        this->BeforeReleaseContext(context);

        auto const bounds = _ids_of_contexts.equal_range(context);
        for (auto itr = bounds.first; itr != bounds.second; ++itr)
            _scripts.erase(itr->second);
    }

    void SwapContext(bool initialize) final override
    {
        this->BeforeSwapContext(initialize);

        _recently_added_ids.clear();
    }

    void RemoveUsedScriptsFromContainer(std::unordered_set<std::string>& scripts) final override
    {
        for (auto const& script : _scripts)
            scripts.erase(script.second->GetName());
    }

    void Unload() final override
    {
        this->BeforeUnload();

        ASSERT(_recently_added_ids.empty(),
            "Recently added script ids should be empty here!");

        _scripts.clear();
        _ids_of_contexts.clear();
    }

    // Adds a database bound script
    void AddScript(ScriptType* script)
    {
        ASSERT(script,
            "Tried to call AddScript with a nullpointer!");
        ASSERT(!sScriptMgr->GetCurrentScriptContext().empty(),
            "Tried to register a script without being in a valid script context!");

        std::unique_ptr<ScriptType> script_ptr(script);

        // Get an ID for the script. An ID only exists if it's a script that is assigned in the database
        // through a script name (or similar).
        if (uint32 const id = sObjectMgr->GetScriptId(script->GetName()))
        {
            // Try to find an existing script.
            for (auto const& stored_script : _scripts)
            {
                // If the script names match...
                if (stored_script.second->GetName() == script->GetName())
                {
                    // If the script is already assigned -> delete it!
                    TC_LOG_ERROR("scripts", "Script '%s' already assigned with the same script name, "
                        "so the script can't work.", script->GetName().c_str());

                    // Error that should be fixed ASAP.
                    sScriptRegistryCompositum->QueueForDelayedDelete(std::move(script_ptr));
                    ABORT();
                    return;
                }
            }

            // If the script isn't assigned -> assign it!
            _scripts.insert(std::make_pair(id, std::move(script_ptr)));
            _ids_of_contexts.insert(std::make_pair(sScriptMgr->GetCurrentScriptContext(), id));
            _recently_added_ids.insert(id);

            sScriptRegistryCompositum->SetScriptNameInContext(script->GetName(),
                sScriptMgr->GetCurrentScriptContext());
        }
        else
        {
            // The script uses a script name from database, but isn't assigned to anything.
            TC_LOG_ERROR("sql.sql", "Script named '%s' does not have a script name assigned in database.",
                script->GetName().c_str());

            // Avoid calling "delete script;" because we are currently in the script constructor
            // In a valid scenario this will not happen because every script has a name assigned in the database
            sScriptRegistryCompositum->QueueForDelayedDelete(std::move(script_ptr));
            return;
        }
    }

    // Gets a script by its ID (assigned by ObjectMgr).
    ScriptType* GetScriptById(uint32 id)
    {
        auto const itr = _scripts.find(id);
        if (itr != _scripts.end())
            return itr->second.get();

        return nullptr;
    }

    ScriptStoreType& GetScripts()
    {
        return _scripts;
    }

protected:
    // Returns the script id's which are registered to a certain context
    std::unordered_set<uint32> GetScriptIDsToRemove(std::string const& context) const
    {
        // Create a set of all ids which are removed
        std::unordered_set<uint32> scripts_to_remove;

        auto const bounds = _ids_of_contexts.equal_range(context);
        for (auto itr = bounds.first; itr != bounds.second; ++itr)
            scripts_to_remove.insert(itr->second);

        return scripts_to_remove;
    }

    std::unordered_set<uint32> const& GetRecentlyAddedScriptIDs() const
    {
        return _recently_added_ids;
    }

private:
    ScriptStoreType _scripts;

    // Scripts of a specific context
    std::unordered_multimap<std::string /*context*/, uint32 /*id*/> _ids_of_contexts;

    // Script id's which were registered recently
    std::unordered_set<uint32> _recently_added_ids;
};

/// This hook is responsible for swapping CommandScript's
/*
template<typename Base>
class ScriptRegistrySwapHooks<CommandScript, Base>
    : public ScriptRegistrySwapHookBase
{
public:
    void BeforeReleaseContext(std::string const& ) final override
    {
        ChatHandler::invalidateCommandTable();
    }

    void BeforeSwapContext(bool ) override
    {
        ChatHandler::invalidateCommandTable();
    }

    void BeforeUnload() final override
    {
        ChatHandler::invalidateCommandTable();
    }
};
*/

// Database unbound script registry
template<typename ScriptType>
class SpecializedScriptRegistry<ScriptType, false>
    : public ScriptRegistryInterface,
    public ScriptRegistrySwapHooks<ScriptType, ScriptRegistry<ScriptType>>
{
    template<typename, typename>
    friend class ScriptRegistrySwapHooks;

public:
    typedef std::unordered_multimap<std::string /*context*/, std::unique_ptr<ScriptType>> ScriptStoreType;
    typedef typename ScriptStoreType::iterator ScriptStoreIteratorType;

    SpecializedScriptRegistry() { }

    void ReleaseContext(std::string const& context) final override
    {
        this->BeforeReleaseContext(context);

        _scripts.erase(context);
    }

    void SwapContext(bool initialize) final override
    {
        this->BeforeSwapContext(initialize);
    }

    void RemoveUsedScriptsFromContainer(std::unordered_set<std::string>& scripts) final override
    {
        for (auto const& script : _scripts)
            scripts.erase(script.second->GetName());
    }

    void Unload() final override
    {
        this->BeforeUnload();

        _scripts.clear();
    }

    // Adds a non database bound script
    void AddScript(ScriptType* script)
    {
        ASSERT(script,
            "Tried to call AddScript with a nullpointer!");
        ASSERT(!sScriptMgr->GetCurrentScriptContext().empty(),
            "Tried to register a script without being in a valid script context!");

        std::unique_ptr<ScriptType> script_ptr(script);

        for (auto const& entry : _scripts)
            if (entry.second.get() == script)
            {
                static_cast<ScriptRegistry<ScriptType>*>(this)->
                    LogDuplicatedScriptPointerError(script, entry.second.get());

                sScriptRegistryCompositum->QueueForDelayedDelete(std::move(script_ptr));
                return;
            }

        // We're dealing with a code-only script, just add it.
        _scripts.insert(std::make_pair(sScriptMgr->GetCurrentScriptContext(), std::move(script_ptr)));
    }

    ScriptStoreType& GetScripts()
    {
        return _scripts;
    }

private:
    ScriptStoreType _scripts;
};

#define TEXT_SOURCE_RANGE   -1000000                        //the amount of entries each text source has available

// Text Maps
std::unordered_map<int32, StringTextData> TextMap;

void LoadOverridenSQLData();

// -- Scripts to be added --

// -- Areatrigger --
extern void AddSC_areatrigger_scripts();
extern void AddSC_SmartScripts();

// -- Outdoors dragons --
extern void AddSC_boss_dragonsofnightmare();

// -- Creature --
extern void AddSC_generic_creature();

// -- Custom --
extern void AddSC_npc_rez();
extern void AddSC_training_dummy();
extern void AddSC_zone_silence();
extern void AddSC_custom_example();
extern void AddSC_custom_gossip_codebox();
extern void AddSC_npc_lottery();
extern void AddSC_theinform();
extern void AddSC_mylittlebombling();
extern void AddSC_firework_controller();
extern void AddSC_npc_interpreter();
extern void AddSC_custom_gnominizer();
extern void AddSC_pack58();
extern void AddSC_beta_utility_npc();

extern void AddSC_catapultmaster();
extern void AddSC_npc_teleporter();
extern void AddSC_npc_teleporter_pvpzone();

// -- GO --
extern void AddSC_go_scripts();

// -- Guard --
extern void AddSC_guards();

// -- Honor --

// -- Item --
extern void AddSC_item_scripts();
extern void AddSC_item_test();

// -- NPC --
extern void AddSC_npc_professions();
extern void AddSC_npcs_special();
extern void AddSC_npc_xp_blocker();

// -- Misc --
extern void AddSC_arena_spectator_script();
extern void AddSC_hallows_end();
extern void AddSC_arenabeastmaster();

//--------------------
//------ ZONE --------

//Alterac Mountains
extern void AddSC_alterac_mountains();

//Alterac Valley
extern void AddSC_AV_Warmasters();
extern void AddSC_boss_balinda();
extern void AddSC_boss_drekthar();
extern void AddSC_boss_galvangar();
extern void AddSC_boss_vanndar();
extern void AddSC_alterac_bowman();

//Arathi Highlands
extern void AddSC_arathi_highlands();

//Ashenvale Forest
extern void AddSC_ashenvale();

//Aunchindoun
//--Auchenai Crypts
extern void AddSC_boss_exarch_maladaar();
extern void AddSC_boss_shirrak_the_dead_watcher();

//--Mana Tombs
extern void AddSC_boss_nexusprince_shaffar();
extern void AddSC_boss_pandemonius();

//--Sekketh Halls
extern void AddSC_boss_darkweaver_syth();
extern void AddSC_boss_talon_king_ikiss();
extern void AddSC_boss_anzu();
extern void AddSC_instance_sethekk_halls();
extern void AddSC_sethekk_halls();

//--Shadow Labyrinth
extern void AddSC_boss_ambassador_hellmaw();
extern void AddSC_boss_blackheart_the_inciter();
extern void AddSC_boss_grandmaster_vorpil();
extern void AddSC_boss_murmur();
extern void AddSC_instance_shadow_labyrinth();

//Azshara
extern void AddSC_boss_azuregos();
extern void AddSC_azshara();

//Azuremyst Isle
extern void AddSC_azuremyst_isle();

//Badlands
//Barrens
extern void AddSC_the_barrens();

//Black Temple
extern void AddSC_black_temple();
extern void AddSC_boss_illidan();
extern void AddSC_boss_shade_of_akama();
extern void AddSC_boss_supremus();
extern void AddSC_boss_gurtogg_bloodboil();
extern void AddSC_boss_mother_shahraz();
extern void AddSC_boss_reliquary_of_souls();
extern void AddSC_boss_teron_gorefiend();
extern void AddSC_boss_najentus();
extern void AddSC_boss_illidari_council();
extern void AddSC_instance_black_temple();

//Blackfathom Depths
extern void AddSC_blackfathom_deeps();
extern void AddSC_boss_gelihast();
extern void AddSC_boss_kelris();
extern void AddSC_boss_aku_mai();
extern void AddSC_instance_blackfathom_deeps();

//Blackrock Depths
extern void AddSC_blackrock_depths();
extern void AddSC_boss_ambassador_flamelash();
extern void AddSC_boss_anubshiah();
extern void AddSC_boss_draganthaurissan();
extern void AddSC_boss_general_angerforge();
extern void AddSC_boss_gorosh_the_dervish();
extern void AddSC_boss_grizzle();
extern void AddSC_boss_high_interrogator_gerstahn();
extern void AddSC_boss_magmus();
extern void AddSC_boss_moira_bronzebeard();
extern void AddSC_boss_tomb_of_seven();
extern void AddSC_instance_blackrock_depths();

//Blackrock Spire
extern void AddSC_boss_drakkisath();
extern void AddSC_boss_halycon();
extern void AddSC_boss_highlordomokk();
extern void AddSC_boss_mothersmolderweb();
extern void AddSC_boss_overlordwyrmthalak();
extern void AddSC_boss_shadowvosh();
extern void AddSC_boss_thebeast();
extern void AddSC_boss_warmastervoone();
extern void AddSC_boss_quatermasterzigris();
extern void AddSC_boss_pyroguard_emberseer();
extern void AddSC_boss_gyth();
extern void AddSC_boss_rend_blackhand();

//Blackwing lair
extern void AddSC_boss_razorgore();
extern void AddSC_boss_vael();
extern void AddSC_boss_broodlord();
extern void AddSC_boss_firemaw();
extern void AddSC_boss_ebonroc();
extern void AddSC_boss_flamegor();
extern void AddSC_boss_chromaggus();
extern void AddSC_boss_nefarian();
extern void AddSC_boss_victor_nefarius();

//Blade's Edge Mountains
extern void AddSC_blades_edge_mountains();
extern void AddSC_shartuul_event();

//Blasted lands
extern void AddSC_boss_kruul();
extern void AddSC_blasted_lands();

//Bloodmyst Isle
extern void AddSC_bloodmyst_isle();

//Burning steppes
extern void AddSC_burning_steppes();

//Caverns of Time
//--Battle for Mt. Hyjal
extern void AddSC_hyjal();
extern void AddSC_boss_archimonde();
extern void AddSC_instance_mount_hyjal();
extern void AddSC_hyjal_trash();
extern void AddSC_boss_rage_winterchill();
extern void AddSC_boss_anetheron();
extern void AddSC_boss_kazrogal();
extern void AddSC_boss_azgalor();

//--Old Hillsbrad
extern void AddSC_boss_captain_skarloc();
extern void AddSC_boss_epoch_hunter();
extern void AddSC_boss_lieutenant_drake();
extern void AddSC_instance_old_hillsbrad();
extern void AddSC_old_hillsbrad();

//--The Dark Portal
extern void AddSC_boss_aeonus();
extern void AddSC_boss_chrono_lord_deja();
extern void AddSC_boss_temporus();
extern void AddSC_dark_portal();
extern void AddSC_instance_dark_portal();

//Coilfang Resevoir
//--Serpent Shrine Cavern
extern void AddSC_boss_fathomlord_karathress();
extern void AddSC_boss_hydross_the_unstable();
extern void AddSC_boss_lady_vashj();
extern void AddSC_boss_leotheras_the_blind();
extern void AddSC_boss_morogrim_tidewalker();
extern void AddSC_instance_serpentshrine_cavern();
extern void AddSC_boss_the_lurker_below();

//--Slave Pens
extern void AddSC_boss_ahune();
extern void AddSC_instance_slave_pens();
extern void AddSC_slave_pens();

//--Steam Vault
extern void AddSC_boss_hydromancer_thespia();
extern void AddSC_boss_mekgineer_steamrigger();
extern void AddSC_boss_warlord_kalithresh();
extern void AddSC_instance_steam_vault();

//--Underbog
extern void AddSC_boss_hungarfen();
extern void AddSC_boss_the_black_stalker();

//Darkshore
extern void AddSC_darkshore();
//Darnassus
//Deadmines
extern void AddSC_instance_deadmines();

//Deadwind pass
//Desolace
extern void AddSC_desolace();

//Dire Maul
extern void AddSC_instance_dire_maul();
extern void AddSC_dire_maul();
extern void AddSC_boss_pusillin();

//Dun Morogh
extern void AddSC_dun_morogh();

//Durotar
extern void AddSC_durotar();

//Duskwood
//Dustwallow marsh
extern void AddSC_dustwallow_marsh();

//Eversong Woods
extern void AddSC_eversong_woods();

//Exodar
//Eastern Plaguelands
extern void AddSC_eastern_plaguelands();

//Elwynn Forest
extern void AddSC_elwynn_forest();

//Felwood
extern void AddSC_felwood();

//Feralas
extern void AddSC_feralas();

//Ghostlands
extern void AddSC_ghostlands();

//Gnomeregan
//Gruul's Lair
extern void AddSC_boss_gruul();
extern void AddSC_boss_high_king_maulgar();
extern void AddSC_instance_gruuls_lair();

//Hellfire Citadel
//--Blood Furnace
extern void AddSC_boss_broggok();
extern void AddSC_boss_kelidan_the_breaker();
extern void AddSC_boss_the_maker();
extern void AddSC_instance_blood_furnace();

//--Magtheridon's Lair
extern void AddSC_boss_magtheridon();
extern void AddSC_instance_magtheridons_lair();

//--Shattered Halls
extern void AddSC_boss_grand_warlock_nethekurse();
extern void AddSC_boss_warbringer_omrogg();
extern void AddSC_boss_warchief_kargath_bladefist();
extern void AddSC_instance_shattered_halls();

//--Ramparts
extern void AddSC_boss_watchkeeper_gargolmar();
extern void AddSC_boss_omor_the_unscarred();
extern void AddSC_boss_vazruden_the_herald();

//Hellfire Peninsula
extern void AddSC_boss_doomlordkazzak();
extern void AddSC_hellfire_peninsula();

//Hillsbrad Foothills
//Hinterlands
extern void AddSC_hinterlands();

//Ironforge
extern void AddSC_ironforge();

//Isle of Quel'Danas
extern void AddSC_isle_of_queldanas();

//Karazhan
extern void AddSC_boss_attumen();
extern void AddSC_boss_curator();
extern void AddSC_boss_maiden_of_virtue();
extern void AddSC_boss_shade_of_aran();
extern void AddSC_boss_malchezaar();
extern void AddSC_boss_terestian_illhoof();
extern void AddSC_boss_moroes();
extern void AddSC_bosses_opera();
extern void AddSC_boss_netherspite();
extern void AddSC_instance_karazhan();
extern void AddSC_karazhan();
extern void AddSC_boss_nightbane();
extern void AddSC_chess_event();

//Loch Modan
extern void AddSC_loch_modan();

//Lower Blackrock Spire
extern void AddSC_instance_blackrock_spire();

// Magister's Terrace
extern void AddSC_boss_felblood_kaelthas();
extern void AddSC_boss_selin_fireheart();
extern void AddSC_boss_vexallus();
extern void AddSC_boss_priestess_delrissa();
extern void AddSC_instance_magisters_terrace();

//Maraudon
extern void AddSC_boss_celebras_the_cursed();
extern void AddSC_boss_landslide();
extern void AddSC_boss_noxxion();
extern void AddSC_boss_ptheradras();

//Molten core
extern void AddSC_boss_lucifron();
extern void AddSC_boss_magmadar();
extern void AddSC_boss_gehennas();
extern void AddSC_boss_garr();
extern void AddSC_boss_baron_geddon();
extern void AddSC_boss_shazzrah();
extern void AddSC_boss_golemagg();
extern void AddSC_boss_sulfuron();
extern void AddSC_boss_majordomo();
extern void AddSC_boss_ragnaros();
extern void AddSC_instance_molten_core();
extern void AddSC_molten_core();

//Moonglade
extern void AddSC_moonglade();

//Mulgore
extern void AddSC_mulgore();

//Nagrand
extern void AddSC_nagrand();

//Naxxramas
extern void AddSC_boss_anubrekhan();
extern void AddSC_boss_maexxna();
extern void AddSC_boss_patchwerk();
extern void AddSC_boss_razuvious();
extern void AddSC_boss_highlord_mograine();
extern void AddSC_boss_kelthuzad();
extern void AddSC_boss_faerlina();
extern void AddSC_boss_loatheb();
extern void AddSC_boss_noth();
extern void AddSC_boss_gluth();
extern void AddSC_boss_sapphiron();
extern void AddSC_boss_four_horsemen();

//Netherstorm
extern void AddSC_netherstorm();

//Onyxia's Lair
extern void AddSC_boss_onyxia();

//Orgrimmar
extern void AddSC_orgrimmar();

//Ragefire Chasm
//Razorfen Downs
extern void AddSC_boss_tuten_kash();
extern void AddSC_boss_plaguemaw_the_rotting();
extern void AddSC_boss_ragglesnout();
extern void AddSC_boss_glutton();
extern void AddSC_boss_mordresh_fire_eye();
extern void AddSC_boss_amnennar_the_coldbringer();
extern void AddSC_razorfen_downs();
extern void AddSC_instance_razorfen_downs();

//Razorfen Kraul
extern void AddSC_razorfen_kraul();
extern void AddSC_instance_razorfen_kraul();

//Redridge Mountains
extern void AddSC_redridge_mountains();

//Ruins of Ahn'Qiraj
extern void AddSC_boss_kurinnaxx();
extern void AddSC_boss_rajaxx();
extern void AddSC_boss_moam();
extern void AddSC_boss_buru();
extern void AddSC_boss_ayamiss();
extern void AddSC_boss_ossirian();
extern void AddSC_instance_ruins_of_ahnqiraj();
//Scarlet Monastery
extern void AddSC_boss_arcanist_doan();
extern void AddSC_boss_azshir_the_sleepless();
extern void AddSC_boss_bloodmage_thalnos();
extern void AddSC_boss_headless_horseman();
extern void AddSC_boss_herod();
extern void AddSC_boss_high_inquisitor_fairbanks();
extern void AddSC_boss_houndmaster_loksey();
extern void AddSC_boss_interrogator_vishas();
extern void AddSC_boss_scorn();
extern void AddSC_instance_scarlet_monastery();
extern void AddSC_boss_mograine_and_whitemane();

//Scholomance
extern void AddSC_boss_darkmaster_gandling();
extern void AddSC_boss_death_knight_darkreaver();
extern void AddSC_boss_theolenkrastinov();
extern void AddSC_boss_illuciabarov();
extern void AddSC_boss_instructormalicia();
extern void AddSC_boss_jandicebarov();
extern void AddSC_boss_kormok();
extern void AddSC_boss_lordalexeibarov();
extern void AddSC_boss_lorekeeperpolkelt();
extern void AddSC_boss_rasfrost();
extern void AddSC_boss_theravenian();
extern void AddSC_boss_vectus();
extern void AddSC_instance_scholomance();
extern void AddSC_scholomance();

//Searing gorge
extern void AddSC_searing_gorge();

//Shadowfang keep
extern void AddSC_shadowfang_keep();
extern void AddSC_instance_shadowfang_keep();

//Shadowmoon Valley
extern void AddSC_boss_doomwalker();
extern void AddSC_shadowmoon_valley();

//Shattrath
extern void AddSC_shattrath_city();

//Silithus
extern void AddSC_silithus();

//Silvermoon
extern void AddSC_silvermoon_city();

//Silverpine forest
extern void AddSC_silverpine_forest();

//Stockade
//Stonetalon mountains
extern void AddSC_stonetalon_mountains();

//Stormwind City
extern void AddSC_stormwind_city();

//Stranglethorn Vale
extern void AddSC_stranglethorn_vale();

//Stratholme
extern void AddSC_boss_magistrate_barthilas();
extern void AddSC_boss_maleki_the_pallid();
extern void AddSC_boss_nerubenkan();
extern void AddSC_boss_cannon_master_willey();
extern void AddSC_boss_baroness_anastari();
extern void AddSC_boss_ramstein_the_gorger();
extern void AddSC_boss_timmy_the_cruel();
extern void AddSC_boss_postmaster_malown();
extern void AddSC_boss_baron_rivendare();
extern void AddSC_boss_dathrohan_balnazzar();
extern void AddSC_boss_order_of_silver_hand();
extern void AddSC_instance_stratholme();
extern void AddSC_stratholme();

//Sunken Temple
extern void AddSC_instance_sunken_temple();
extern void AddSC_sunken_temple();

//Sunwell Plateau
extern void AddSC_instance_sunwell_plateau();
extern void AddSC_boss_kalecgos();
extern void AddSC_boss_brutallus();
extern void AddSC_boss_felmyst();
extern void AddSC_boss_eredar_twins();
extern void AddSC_boss_muru();
extern void AddSC_boss_kiljaeden_new();
extern void AddSC_sunwell_plateau();

//Swamp of Sorrows
extern void AddSC_swamp_of_sorrows();

//Tanaris
extern void AddSC_tanaris();

//Teldrassil
extern void AddSC_teldrassil();

//Tempest Keep
//--Arcatraz
extern void AddSC_arcatraz();
extern void AddSC_boss_harbinger_skyriss();
extern void AddSC_instance_arcatraz();

//--Botanica
extern void AddSC_boss_high_botanist_freywinn();
extern void AddSC_boss_laj();
extern void AddSC_boss_warp_splinter();

//--The Eye
extern void AddSC_boss_alar();
extern void AddSC_boss_kaelthas();
extern void AddSC_boss_void_reaver();
extern void AddSC_boss_high_astromancer_solarian();
extern void AddSC_instance_the_eye();
extern void AddSC_the_eye();

//--The Mechanar
extern void AddSC_boss_mechanolord_capacitus();
extern void AddSC_boss_gatewatcher_iron_hand();
extern void AddSC_boss_nethermancer_sepethrea();
extern void AddSC_boss_pathaleon_the_calculator();
extern void AddSC_instance_mechanar();
extern void AddSC_mechanar();

//Temple of ahn'qiraj
extern void AddSC_boss_cthun();
extern void AddSC_boss_fankriss();
extern void AddSC_boss_huhuran();
extern void AddSC_bug_trio();
extern void AddSC_boss_sartura();
extern void AddSC_boss_skeram();
extern void AddSC_boss_twinemperors();
extern void AddSC_mob_anubisath_sentinel();
extern void AddSC_instance_temple_of_ahnqiraj();
extern void AddSC_boss_ouro();

//Terokkar Forest
extern void AddSC_terokkar_forest();

//Thousand Needles
extern void AddSC_thousand_needles();

//Thunder Bluff
extern void AddSC_thunder_bluff();

//The Balance of Light and Shadow - Epic priest quest
extern void AddSC_the_balance_of_light_and_shadow();

//Tirisfal Glades
extern void AddSC_tirisfal_glades();

//Uldaman
extern void AddSC_boss_archaedas();
extern void AddSC_boss_ironaya();
extern void AddSC_uldaman();
extern void AddSC_instance_uldaman();

//Undercity
extern void AddSC_undercity();

//Un'Goro Crater
extern void AddSC_ungoro_crater();

//Upper blackrock spire
//Wailing caverns
extern void AddSC_instance_wailing_caverns();
extern void AddSC_wailing_caverns();

//Western plaguelands
extern void AddSC_western_plaguelands();

//Westfall
extern void AddSC_westfall();

//Wetlands
extern void AddSC_wetlands();

//Winterspring
extern void AddSC_winterspring();

//Zangarmarsh
extern void AddSC_zangarmarsh();

//Zul'Farrak
extern void AddSC_zulfarrak();
extern void AddSC_instance_zulfarrak();

//Zul'Gurub
extern void AddSC_boss_jeklik();
extern void AddSC_boss_venoxis();
extern void AddSC_boss_marli();
extern void AddSC_boss_mandokir();
extern void AddSC_boss_gahzranka();
extern void AddSC_boss_thekal();
extern void AddSC_boss_arlokk();
extern void AddSC_boss_jindo();
extern void AddSC_boss_hakkar();
extern void AddSC_boss_grilek();
extern void AddSC_boss_hazzarah();
extern void AddSC_boss_renataki();
extern void AddSC_boss_wushoolay();
extern void AddSC_instance_zulgurub();

//Zul'Aman
extern void AddSC_boss_akilzon();
extern void AddSC_boss_halazzi();
extern void AddSC_boss_hex_lord_malacrass();
extern void AddSC_boss_janalai();
extern void AddSC_boss_nalorakk();
extern void AddSC_boss_zuljin();
extern void AddSC_instance_zulaman();
extern void AddSC_zulaman();

//Spells
extern void AddSC_generic_spell_scripts();

// -------------------
void ScriptMgr::LoadDatabase()
{
    //***Preform all DB queries here***
    QueryResult result;

    // Drop Existing Text Map, only done once and we are ready to add data from multiple sources.
    TextMap.clear();

    // Load OLDScript Text
    TC_LOG_INFO("server.loading","TSCR: Loading Script Texts...");
    LoadTrinityStrings(WorldDatabase,"script_texts",TEXT_SOURCE_RANGE,1+(TEXT_SOURCE_RANGE*2));

    // Gather Additional data from Script Texts
    result = WorldDatabase.PQuery("SELECT entry, sound, type, language, emote FROM script_texts");

    TC_LOG_INFO("server.loading","TSCR: Loading Script Texts additional data...");
    if (result)
    {
        uint32 count = 0;

        do
        {
            Field* fields = result->Fetch();
            StringTextData temp;

            int32 i             = fields[0].GetInt32();
            temp.SoundId        = fields[1].GetInt32();
            temp.Type           = fields[2].GetInt8();
            temp.Language       = fields[3].GetInt8();
            temp.Emote          = fields[4].GetInt16();

            if (i >= 0)
            {
                error_db_log("TSCR: Entry %i in table `script_texts` is not a negative value.",i);
                continue;
            }

            if (i > TEXT_SOURCE_RANGE || i <= TEXT_SOURCE_RANGE*2)
            {
                error_db_log("TSCR: Entry %i in table `script_texts` is out of accepted entry range for table.",i);
                continue;
            }

            if (temp.SoundId)
            {
                if (!GetSoundEntriesStore()->LookupEntry(temp.SoundId))
                    error_db_log("TSCR: Entry %i in table `script_texts` has soundId %u but sound does not exist.",i,temp.SoundId);
            }

            if (!GetLanguageDescByID(temp.Language))
                error_db_log("TSCR: Entry %i in table `script_texts` using Language %u but Language does not exist.",i,temp.Language);

            if (temp.Type >= CHAT_TYPE_END)
                error_db_log("TSCR: Entry %i in table `script_texts` has Type %u but this Chat Type does not exist.",i,temp.Type);

            TextMap[i] = temp;
            ++count;
        } while (result->NextRow());

        TC_LOG_INFO("server.loading","\n>> TSCR: Loaded %u additional Script Texts data.", count);
    }else
    {
        TC_LOG_INFO("server.loading","\n>> Loaded 0 additional Script Texts data. DB table `script_texts` is empty.");
    }
}

// Utility macros to refer to the script registry.
#define SCR_REG_MAP(T) ScriptRegistry<T>::ScriptStoreType
#define SCR_REG_ITR(T) ScriptRegistry<T>::ScriptStoreIteratorType
#define SCR_REG_LST(T) ScriptRegistry<T>::Instance()->GetScripts()

// Utility macros for looping over scripts.
#define FOR_SCRIPTS(T, C, E) \
    if (SCR_REG_LST(T).empty()) \
        return; \
	\
    for (SCR_REG_ITR(T) C = SCR_REG_LST(T).begin(); \
        C != SCR_REG_LST(T).end(); ++C)
	\
#define FOR_SCRIPTS_RET(T, C, E, R) \
    if (SCR_REG_LST(T).empty()) \
        return R; \
	\
    for (SCR_REG_ITR(T) C = SCR_REG_LST(T).begin(); \
        C != SCR_REG_LST(T).end(); ++C)

#define FOREACH_SCRIPT(T) \
    FOR_SCRIPTS(T, itr, end) \
    itr->second

// Utility macros for finding specific scripts.
#define GET_SCRIPT(T, I, V) \
    T* V = ScriptRegistry<T>::GetScriptById(I); \
    if (!V) \
        return;
#define GET_SCRIPT_RET(T, I, V, R) \
    T* V = ScriptRegistry<T>::GetScriptById(I); \
    if (!V) \
        return R;

struct TSpellSummary
{
    uint8 Targets;                                          // set of enum SelectSpellTarget
    uint8 Effects;                                          // set of enum SelectEffect
} *SpellSummary;

ScriptObject::ScriptObject(const char* name) : _name(name)
{
    sScriptMgr->IncreaseScriptCount();
}

ScriptObject::~ScriptObject()
{
    sScriptMgr->DecreaseScriptCount();
}

ScriptMgr::ScriptMgr()
    : _scriptCount(0), _script_loader_callback(nullptr)
{
    
}

ScriptMgr::~ScriptMgr()
{
}

void ScriptMgr::Initialize(char const* cfg_file)
{
    ASSERT(sSpellMgr->GetSpellInfo(SPELL_HOTSWAP_VISUAL_SPELL_EFFECT)
        && "Reload hotswap spell effect for creatures isn't valid!");

    uint32 oldMSTime = GetMSTime();

    //Load database (must be called after TScriptConfig.SetSource). In case it failed, no need to even try load.
    LoadDatabase();

    //Trinity Script startup
    TC_LOG_INFO("server.loading", "Loading C++ scripts");

    FillSpellSummary();

    // Load core scripts
    SetScriptContext(GetNameOfStaticContext());

    // Load all static linked scripts through the script loader function.
    ASSERT(_script_loader_callback,
        "Script loader callback wasn't registered!");
    _script_loader_callback();

    // Initialize all dynamic scripts
    // and finishes the context switch to do
    // bulk loading
    sScriptReloadMgr->Initialize();

    // Loads all scripts from the current context
    sScriptMgr->SwapScriptContext(true);

    // Print unused script names.
    std::unordered_set<std::string> unusedScriptNames(
        sObjectMgr->GetAllScriptNames().begin(),
        sObjectMgr->GetAllScriptNames().end());

    // Remove the used scripts from the given container.
    sScriptRegistryCompositum->RemoveUsedScriptsFromContainer(unusedScriptNames);

    for (std::string const& scriptName : unusedScriptNames)
    {
        // Avoid complaining about empty script names since the
        // script name container contains a placeholder as the 0 element.
        if (scriptName.empty())
            continue;

        TC_LOG_ERROR("sql.sql", "ScriptName '%s' exists in database, "
            "but no core script found!", scriptName.c_str());
    }

    // -- Scripts to be added --

    // -- Areatrigger --
    AddSC_areatrigger_scripts();
    AddSC_SmartScripts();

    // -- Outdoors Dragons --
    AddSC_boss_dragonsofnightmare();

    // -- Creature --
    AddSC_generic_creature();

    // -- Custom --
    AddSC_npc_rez();
    AddSC_training_dummy();
    AddSC_zone_silence();
    AddSC_custom_example();
    AddSC_custom_gossip_codebox();
    AddSC_npc_lottery();
    AddSC_theinform();
    AddSC_mylittlebombling();
    AddSC_firework_controller();
    AddSC_npc_interpreter();
    AddSC_custom_gnominizer();
    AddSC_pack58();
    AddSC_beta_utility_npc();

    AddSC_npc_teleporter();
    AddSC_npc_teleporter_pvpzone();
    AddSC_catapultmaster();

    // -- GO --
    AddSC_go_scripts();

    // -- Guard --
    AddSC_guards();

    // -- Honor --

    // -- Item --
    AddSC_item_scripts();
    AddSC_item_test();

    // -- NPC --
    AddSC_npc_professions();
    AddSC_npcs_special();
    AddSC_npc_xp_blocker();

    // -- Misc --
    AddSC_arena_spectator_script();
    AddSC_hallows_end();
    AddSC_arenabeastmaster();

    //--------------------
    //------ ZONE --------

    //Alterac Mountains
    AddSC_alterac_mountains();
    
    //Alterac Valley
    AddSC_AV_Warmasters();
    AddSC_boss_balinda();
    AddSC_boss_drekthar();
    AddSC_boss_galvangar();
    AddSC_boss_vanndar();
    AddSC_alterac_bowman();

    //Arathi Highlands
    AddSC_arathi_highlands();

    //Ashenvale Forest
    AddSC_ashenvale();

    //Aunchindoun
    //--Auchenai Crypts
    AddSC_boss_exarch_maladaar();
    AddSC_boss_shirrak_the_dead_watcher();

    //--Mana Tombs
    AddSC_boss_nexusprince_shaffar();
    AddSC_boss_pandemonius();

    //--Sekketh Halls
    AddSC_boss_darkweaver_syth();
    AddSC_boss_talon_king_ikiss();
    AddSC_boss_anzu();
    AddSC_instance_sethekk_halls();
    AddSC_sethekk_halls();

    //--Shadow Labyrinth
    AddSC_boss_ambassador_hellmaw();
    AddSC_boss_blackheart_the_inciter();
    AddSC_boss_grandmaster_vorpil();
    AddSC_boss_murmur();
    AddSC_instance_shadow_labyrinth();

    //Azshara
    AddSC_boss_azuregos();
    AddSC_azshara();

    //Azuremyst Isle
    AddSC_azuremyst_isle();

    //Badlands
    //Barrens
    AddSC_the_barrens();

    //Black Temple
    AddSC_black_temple();
    AddSC_boss_illidan();
    AddSC_boss_shade_of_akama();
    AddSC_boss_supremus();
    AddSC_boss_gurtogg_bloodboil();
    AddSC_boss_mother_shahraz();
    AddSC_boss_reliquary_of_souls();
    AddSC_boss_teron_gorefiend();
    AddSC_boss_najentus();
    AddSC_boss_illidari_council();
    AddSC_instance_black_temple();

    //Blackfathom Depths
    AddSC_blackfathom_deeps();
    AddSC_boss_gelihast();
    AddSC_boss_kelris();
    AddSC_boss_aku_mai();
    AddSC_instance_blackfathom_deeps();

    //Blackrock Depths
    AddSC_blackrock_depths();
    AddSC_boss_ambassador_flamelash();
    AddSC_boss_anubshiah();
    AddSC_boss_draganthaurissan();
    AddSC_boss_general_angerforge();
    AddSC_boss_gorosh_the_dervish();
    AddSC_boss_grizzle();
    AddSC_boss_high_interrogator_gerstahn();
    AddSC_boss_magmus();
    AddSC_boss_moira_bronzebeard();
    AddSC_boss_tomb_of_seven();
    AddSC_instance_blackrock_depths();

    //Blackrock Spire
    AddSC_boss_drakkisath();
    AddSC_boss_halycon();
    AddSC_boss_highlordomokk();
    AddSC_boss_mothersmolderweb();
    AddSC_boss_overlordwyrmthalak();
    AddSC_boss_shadowvosh();
    AddSC_boss_thebeast();
    AddSC_boss_warmastervoone();
    AddSC_boss_quatermasterzigris();
    AddSC_boss_pyroguard_emberseer();
    AddSC_boss_gyth();
    AddSC_boss_rend_blackhand();
    AddSC_instance_blackrock_spire();

    //Blackwing lair
    AddSC_boss_razorgore();
    AddSC_boss_vael();
    AddSC_boss_broodlord();
    AddSC_boss_firemaw();
    AddSC_boss_ebonroc();
    AddSC_boss_flamegor();
    AddSC_boss_chromaggus();
    AddSC_boss_nefarian();
    AddSC_boss_victor_nefarius();

    //Blade's Edge Mountains
    AddSC_blades_edge_mountains();
    AddSC_shartuul_event();

    //Blasted lands
    AddSC_boss_kruul();
    AddSC_blasted_lands();

    //Bloodmyst Isle
    AddSC_bloodmyst_isle();

    //Burning steppes
    AddSC_burning_steppes();

    //Caverns of Time
    //--Battle for Mt. Hyjal
    AddSC_hyjal();
    AddSC_boss_archimonde();
    AddSC_instance_mount_hyjal();
    AddSC_hyjal_trash();
    AddSC_boss_rage_winterchill();
    AddSC_boss_anetheron();
    AddSC_boss_kazrogal();
    AddSC_boss_azgalor();

    //--Old Hillsbrad
    AddSC_boss_captain_skarloc();
    AddSC_boss_epoch_hunter();
    AddSC_boss_lieutenant_drake();
    AddSC_instance_old_hillsbrad();
    AddSC_old_hillsbrad();

    //--The Dark Portal
    AddSC_boss_aeonus();
    AddSC_boss_chrono_lord_deja();
    AddSC_boss_temporus();
    AddSC_dark_portal();
    AddSC_instance_dark_portal();

    //Coilfang Resevoir
    //--Serpent Shrine Cavern
    AddSC_boss_fathomlord_karathress();
    AddSC_boss_hydross_the_unstable();
    AddSC_boss_lady_vashj();
    AddSC_boss_leotheras_the_blind();
    AddSC_boss_morogrim_tidewalker();
    AddSC_instance_serpentshrine_cavern();
    AddSC_boss_the_lurker_below();

    //--Slave Pens
    AddSC_boss_ahune();
    AddSC_instance_slave_pens();
    AddSC_slave_pens();
    
    //--Steam Vault
    AddSC_boss_hydromancer_thespia();
    AddSC_boss_mekgineer_steamrigger();
    AddSC_boss_warlord_kalithresh();
    AddSC_instance_steam_vault();

    //--Underbog
    AddSC_boss_hungarfen();
    AddSC_boss_the_black_stalker();

    //Darkshore
    AddSC_darkshore();
    //Darnassus
    //Deadmines
    AddSC_instance_deadmines();

    //Deadwind pass
    //Desolace
    AddSC_desolace();
    
    //Dire Maul
    AddSC_instance_dire_maul();
    AddSC_dire_maul();
    AddSC_boss_pusillin();
    
    //Dun Morogh
    AddSC_dun_morogh();

    //Durotar
    AddSC_durotar();
    
    //Duskwood
    //Dustwallow marsh
    AddSC_dustwallow_marsh();

    //Eversong Woods
    AddSC_eversong_woods();

    //Exodar
    //Eastern Plaguelands
    AddSC_eastern_plaguelands();

    //Elwynn Forest
    AddSC_elwynn_forest();

    //Felwood
    AddSC_felwood();

    //Feralas
    AddSC_feralas();

    //Ghostlands
    AddSC_ghostlands();

    //Gnomeregan
    //Gruul's Lair
    AddSC_boss_gruul();
    AddSC_boss_high_king_maulgar();
    AddSC_instance_gruuls_lair();

    //Hellfire Citadel
    //--Blood Furnace
    AddSC_boss_broggok();
    AddSC_boss_kelidan_the_breaker();
    AddSC_boss_the_maker();
    AddSC_instance_blood_furnace();

    //--Magtheridon's Lair
    AddSC_boss_magtheridon();
    AddSC_instance_magtheridons_lair();

    //--Shattered Halls
    AddSC_boss_grand_warlock_nethekurse();
    AddSC_boss_warbringer_omrogg();
    AddSC_boss_warchief_kargath_bladefist();
    AddSC_instance_shattered_halls();

    //--Ramparts
    AddSC_boss_watchkeeper_gargolmar();
    AddSC_boss_omor_the_unscarred();
    AddSC_boss_vazruden_the_herald();

    //Hellfire Peninsula
    AddSC_boss_doomlordkazzak();
    AddSC_hellfire_peninsula();

    //Hillsbrad Foothills
    //Hinterlands
    AddSC_hinterlands();
    
    //Ironforge
    AddSC_ironforge();

    //Isle of Quel'Danas
    AddSC_isle_of_queldanas();

    //Karazhan
    AddSC_boss_attumen();
    AddSC_boss_curator();
    AddSC_boss_maiden_of_virtue();
    AddSC_boss_shade_of_aran();
    AddSC_boss_malchezaar();
    AddSC_boss_terestian_illhoof();
    AddSC_boss_moroes();
    AddSC_bosses_opera();
    AddSC_boss_netherspite();
    AddSC_instance_karazhan();
    AddSC_karazhan();
    AddSC_boss_nightbane();
    AddSC_chess_event();

    //Loch Modan
    AddSC_loch_modan();

    //Lower Blackrock Spire

    // Magister's Terrace
    AddSC_boss_felblood_kaelthas();
    AddSC_boss_selin_fireheart();
    AddSC_boss_vexallus();
    AddSC_boss_priestess_delrissa();
    AddSC_instance_magisters_terrace();

    //Maraudon
    AddSC_boss_celebras_the_cursed();
    AddSC_boss_landslide();
    AddSC_boss_noxxion();
    AddSC_boss_ptheradras();

    //Molten core
    AddSC_boss_lucifron();
    AddSC_boss_magmadar();
    AddSC_boss_gehennas();
    AddSC_boss_garr();
    AddSC_boss_baron_geddon();
    AddSC_boss_shazzrah();
    AddSC_boss_golemagg();
    AddSC_boss_sulfuron();
    AddSC_boss_majordomo();
    AddSC_boss_ragnaros();
    AddSC_instance_molten_core();
    AddSC_molten_core();

    //Moonglade
    AddSC_moonglade();

    //Mulgore
    AddSC_mulgore();

    //Nagrand
    AddSC_nagrand();

    //Naxxramas
    AddSC_boss_anubrekhan();
    AddSC_boss_maexxna();
    AddSC_boss_patchwerk();
    AddSC_boss_razuvious();
    AddSC_boss_highlord_mograine();
    AddSC_boss_kelthuzad();
    AddSC_boss_faerlina();
    AddSC_boss_loatheb();
    AddSC_boss_noth();
    AddSC_boss_gluth();
    AddSC_boss_sapphiron();
    AddSC_boss_four_horsemen();

    //Netherstorm
    AddSC_netherstorm();

    //Onyxia's Lair
    AddSC_boss_onyxia();

    //Orgrimmar
    AddSC_orgrimmar();

    //Ragefire Chasm
    //Razorfen Downs
    AddSC_boss_tuten_kash();
    AddSC_boss_plaguemaw_the_rotting();
    AddSC_boss_ragglesnout();
    AddSC_boss_glutton();
    AddSC_boss_mordresh_fire_eye();
    AddSC_instance_razorfen_downs();
    AddSC_boss_amnennar_the_coldbringer();
    AddSC_razorfen_downs();

    //Razorfen Kraul
    AddSC_razorfen_kraul();
    AddSC_instance_razorfen_kraul();

    //Redridge Mountains
    AddSC_redridge_mountains();
    
    //Ruins of Ahn'Qiraj
    AddSC_boss_kurinnaxx();
    AddSC_boss_rajaxx();
    AddSC_boss_moam();
    AddSC_boss_buru();
    AddSC_boss_ayamiss();
    AddSC_boss_ossirian();
    AddSC_instance_ruins_of_ahnqiraj();
    //Scarlet Monastery
    AddSC_boss_arcanist_doan();
    AddSC_boss_azshir_the_sleepless();
    AddSC_boss_bloodmage_thalnos();
    AddSC_boss_headless_horseman();
    AddSC_boss_herod();
    AddSC_boss_high_inquisitor_fairbanks();
    AddSC_boss_houndmaster_loksey();
    AddSC_boss_interrogator_vishas();
    AddSC_boss_scorn();
    AddSC_instance_scarlet_monastery();
    AddSC_boss_mograine_and_whitemane();

    //Scholomance
    AddSC_boss_darkmaster_gandling();
    AddSC_boss_death_knight_darkreaver();
    AddSC_boss_theolenkrastinov();
    AddSC_boss_illuciabarov();
    AddSC_boss_instructormalicia();
    AddSC_boss_jandicebarov();
    AddSC_boss_kormok();
    AddSC_boss_lordalexeibarov();
    AddSC_boss_lorekeeperpolkelt();
    AddSC_boss_rasfrost();
    AddSC_boss_theravenian();
    AddSC_boss_vectus();
    AddSC_instance_scholomance();
    AddSC_scholomance();

    //Searing gorge
    AddSC_searing_gorge();

    //Shadowfang keep
    AddSC_shadowfang_keep();
    AddSC_instance_shadowfang_keep();

    //Shadowmoon Valley
    AddSC_boss_doomwalker();
    AddSC_shadowmoon_valley();

    //Shattrath
    AddSC_shattrath_city();

    //Silithus
    AddSC_silithus();

    //Silvermoon
    AddSC_silvermoon_city();

    //Silverpine forest
    AddSC_silverpine_forest();

    //Stockade
    //Stonetalon mountains
    AddSC_stonetalon_mountains();

    //Stormwind City
    AddSC_stormwind_city();

    //Stranglethorn Vale
    AddSC_stranglethorn_vale();

    //Stratholme
    AddSC_boss_magistrate_barthilas();
    AddSC_boss_maleki_the_pallid();
    AddSC_boss_nerubenkan();
    AddSC_boss_cannon_master_willey();
    AddSC_boss_baroness_anastari();
    AddSC_boss_ramstein_the_gorger();
    AddSC_boss_timmy_the_cruel();
    AddSC_boss_postmaster_malown();
    AddSC_boss_baron_rivendare();
    AddSC_boss_dathrohan_balnazzar();
    AddSC_boss_order_of_silver_hand();
    AddSC_instance_stratholme();
    AddSC_stratholme();

    //Sunken Temple
    AddSC_instance_sunken_temple();
    AddSC_sunken_temple();

    //Sunwell Plateau
    AddSC_instance_sunwell_plateau();
    AddSC_boss_kalecgos();
    AddSC_boss_brutallus();
    AddSC_boss_felmyst();
    AddSC_boss_eredar_twins();
    AddSC_boss_muru();
    AddSC_boss_kiljaeden_new();
    AddSC_sunwell_plateau();
    
    //Swamp of Sorrows
    AddSC_swamp_of_sorrows();

    //Tanaris
    AddSC_tanaris();

    //Teldrassil
    AddSC_teldrassil();
    
    //Tempest Keep
    //--Arcatraz
    AddSC_arcatraz();
    AddSC_boss_harbinger_skyriss();
    AddSC_instance_arcatraz();

    //--Botanica
    AddSC_boss_high_botanist_freywinn();
    AddSC_boss_laj();
    AddSC_boss_warp_splinter();

    //--The Eye
    AddSC_boss_alar();
    AddSC_boss_kaelthas();
    AddSC_boss_void_reaver();
    AddSC_boss_high_astromancer_solarian();
    AddSC_instance_the_eye();
    AddSC_the_eye();

    //--The Mechanar
    AddSC_boss_mechanolord_capacitus();
    AddSC_boss_gatewatcher_iron_hand();
    AddSC_boss_nethermancer_sepethrea();
    AddSC_boss_pathaleon_the_calculator();
    AddSC_instance_mechanar();
    AddSC_mechanar();

    //Temple of ahn'qiraj
    AddSC_boss_cthun();
    AddSC_boss_fankriss();
    AddSC_boss_huhuran();
    AddSC_bug_trio();
    AddSC_boss_sartura();
    AddSC_boss_skeram();
    AddSC_boss_twinemperors();
    AddSC_mob_anubisath_sentinel();
    AddSC_instance_temple_of_ahnqiraj();
    AddSC_boss_ouro();

    //Terokkar Forest
    AddSC_terokkar_forest();

    //Thousand Needles
    AddSC_thousand_needles();

    //Thunder Bluff
    AddSC_thunder_bluff();

    // The Balance of Light and Shadow - Epic priest quest
    AddSC_the_balance_of_light_and_shadow();
    
    //Tirisfal Glades
    AddSC_tirisfal_glades();

    //Uldaman
    AddSC_boss_archaedas();
    AddSC_boss_ironaya();
    AddSC_uldaman();
    AddSC_instance_uldaman();

    //Undercity
    AddSC_undercity();

    //Un'Goro Crater
    AddSC_ungoro_crater();

    //Upper blackrock spire
    //Wailing caverns
    AddSC_instance_wailing_caverns();
    AddSC_wailing_caverns();

    //Western plaguelands
    AddSC_western_plaguelands();

    //Westfall
    AddSC_westfall();

    //Wetlands
    AddSC_wetlands();
    
    //Winterspring
    AddSC_winterspring();

    //Zangarmarsh
    AddSC_zangarmarsh();

    //Zul'Farrak
    AddSC_zulfarrak();
    AddSC_instance_zulfarrak();

    //Zul'Gurub
    AddSC_boss_jeklik();
    AddSC_boss_venoxis();
    AddSC_boss_marli();
    AddSC_boss_mandokir();
    AddSC_boss_gahzranka();
    AddSC_boss_thekal();
    AddSC_boss_arlokk();
    AddSC_boss_jindo();
    AddSC_boss_hakkar();
    AddSC_boss_grilek();
    AddSC_boss_hazzarah();
    AddSC_boss_renataki();
    AddSC_boss_wushoolay();
    AddSC_instance_zulgurub();

    //Zul'Aman
    AddSC_boss_akilzon();
    AddSC_boss_halazzi();
    AddSC_boss_hex_lord_malacrass();
    AddSC_boss_janalai();
    AddSC_boss_nalorakk();
    AddSC_boss_zuljin();
    AddSC_instance_zulaman();
    AddSC_zulaman();

    //Spells
    AddSC_generic_spell_scripts();

    // -------------------

    TC_LOG_INFO("server.loading",">> Load Overriden SQL Data.");
    LoadOverridenSQLData();

    TC_LOG_INFO("server.loading", ">> Loaded %u C++ scripts in %u ms",
        GetScriptCount(), GetMSTimeDiffToNow(oldMSTime));
}


void ScriptMgr::SetScriptContext(std::string const& context)
{
    _currentContext = context;
}

void ScriptMgr::SwapScriptContext(bool initialize)
{
    sScriptRegistryCompositum->SwapContext(initialize);
    _currentContext.clear();
}

std::string const& ScriptMgr::GetNameOfStaticContext()
{
    static std::string const name = "___static___";
    return name;
}

void ScriptMgr::ReleaseScriptContext(std::string const& context)
{
    sScriptRegistryCompositum->ReleaseContext(context);
}

std::shared_ptr<ModuleReference>
ScriptMgr::AcquireModuleReferenceOfScriptName(std::string const& scriptname) const
{
#ifdef TRINITY_API_USE_DYNAMIC_LINKING
    // Returns the reference to the module of the given scriptname
    return ScriptReloadMgr::AcquireModuleReferenceOfContext(
        sScriptRegistryCompositum->GetScriptContextOfScriptName(scriptname));
#else
    (void)scriptname;
    // Something went wrong when this function is used in
    // a static linked context.
    WPAbort();
#endif // #ifndef TRINITY_API_USE_DYNAMIC_LINKING
}

void ScriptMgr::Unload()
{
    sScriptRegistryCompositum->Unload();

    delete[] SpellSummary;
    //TC delete[] UnitAI::AISpellInfo;
}

//*********************************
//*** Functions used globally ***

//send text from script_texts table. Only there for retrocompatibity, use Unit::Say/Yell/Whispers ... functions instead.
void DoScriptText(int32 textEntry, Unit* pSource, Unit* target)
{
    if (!pSource)
    {
        error_log("TSCR: DoScriptText entry %i, invalid Source pointer.",textEntry);
        return;
    }

    if (textEntry >= 0)
    {
        error_log("TSCR: DoScriptText with source entry %u (TypeId=%u, guid=%u) attempts to process text entry %i, but text entry must be negative.",pSource->GetEntry(),pSource->GetTypeId(),pSource->GetGUIDLow(),textEntry);
        return;
    }

    auto i = TextMap.find(textEntry);

    if (i == TextMap.end())
    {
        error_log("TSCR: DoScriptText with source entry %u (TypeId=%u, guid=%u) could not find text entry %i.",pSource->GetEntry(),pSource->GetTypeId(),pSource->GetGUIDLow(),textEntry);
        return;
    }

    if((*i).second.SoundId)
    {
        if( GetSoundEntriesStore()->LookupEntry((*i).second.SoundId) )
        {
            pSource->PlayDirectSound((*i).second.SoundId);
        }
        else
            error_log("TSCR: DoScriptText entry %i tried to process invalid sound id %u.",textEntry,(*i).second.SoundId);
    }

    if((*i).second.Emote)
    {
        if (pSource->GetTypeId() == TYPEID_UNIT || pSource->GetTypeId() == TYPEID_PLAYER)
        {
            ((Unit*)pSource)->HandleEmoteCommand((*i).second.Emote);
        }
        else
            error_log("TSCR: DoScriptText entry %i tried to process emote for invalid TypeId (%u).",textEntry,pSource->GetTypeId());
    }

    switch((*i).second.Type)
    {
        case CHAT_TYPE_SAY:
            pSource->old_Say(textEntry, (*i).second.Language, target ? target->GetGUID() : 0);
            break;
        case CHAT_TYPE_YELL:
            pSource->old_Yell(textEntry, (*i).second.Language, target ? target->GetGUID() : 0);
            break;
        case CHAT_TYPE_TEXT_EMOTE:
            pSource->old_TextEmote(textEntry, target ? target->GetGUID() : 0);
            break;
        case CHAT_TYPE_BOSS_EMOTE:
            pSource->old_TextEmote(textEntry, target ? target->GetGUID() : 0, true);
            break;
        case CHAT_TYPE_WHISPER:
            {
                if (target && target->GetTypeId() == TYPEID_PLAYER)
                    pSource->old_Whisper(textEntry, target->GetGUID());
                else error_log("TSCR: DoScriptText entry %i cannot whisper without target unit (TYPEID_PLAYER).", textEntry);
            }break;
        case CHAT_TYPE_BOSS_WHISPER:
            {
                if (target && target->GetTypeId() == TYPEID_PLAYER)
                    pSource->old_Whisper(textEntry, target->GetGUID(), true);
                else error_log("TSCR: DoScriptText entry %i cannot whisper without target unit (TYPEID_PLAYER).", textEntry);
            }break;
    }
}

//*********************************
//*** Functions used internally ***

void ScriptMgr::RegisterOLDScript(OLDScript*& script)
{
    auto  cScript = new CreatureScript(script->Name.c_str());
    cScript->baseScript = script;
    script = nullptr; //CreatureScript object now own the script pointer, remove it for caller
}

#define SCR_MAP_BGN(M, V, I, E, C, T) \
    if (V->GetEntry() && V->GetEntry()->T()) \
    { \
        FOR_SCRIPTS(M, I, E) \
        { \
            MapEntry const* C = I->second->GetEntry(); \
            if (!C) \
                continue; \
            if (C->MapID == V->GetId()) \
            {

#define SCR_MAP_END \
                return; \
            } \
        } \
    }


template<typename T, typename F>
void CreateSpellOrAuraScripts(uint32 spellId, std::list<T*>& scriptVector, F&& extractor)
{
    SpellScriptsBounds bounds = sObjectMgr->GetSpellScriptsBounds(spellId);

    for (SpellScriptsContainer::iterator itr = bounds.first; itr != bounds.second; ++itr)
    {
        // When the script is disabled continue with the next one
        if (!itr->second.second)
            continue;

        SpellScriptLoader* tmpscript = sScriptMgr->GetSpellScriptLoader(itr->second.first);
        if (!tmpscript)
            continue;

        T* script = (*tmpscript.*extractor)();

        if (!script)
            continue;

        script->_Init(&tmpscript->GetName(), spellId);

        scriptVector.push_back(script);
    }
}

void ScriptMgr::CreateSpellScripts(uint32 spellId, std::list<SpellScript*>& scriptVector)
{
    CreateSpellOrAuraScripts(spellId, scriptVector, &SpellScriptLoader::GetSpellScript);
}

void ScriptMgr::OnCreateMap(Map* map)
{
    ASSERT(map);

    SCR_MAP_BGN(WorldMapScript, map, itr, end, entry, IsWorldMap);
    itr->second->OnCreate(map);
    SCR_MAP_END;

    SCR_MAP_BGN(InstanceMapScript, map, itr, end, entry, IsDungeon);
    itr->second->OnCreate((InstanceMap*)map);
    SCR_MAP_END;

    SCR_MAP_BGN(BattlegroundMapScript, map, itr, end, entry, IsBattleground);
    itr->second->OnCreate((BattlegroundMap*)map);
    SCR_MAP_END;
}

void ScriptMgr::OnDestroyMap(Map* map)
{
    ASSERT(map);

    SCR_MAP_BGN(WorldMapScript, map, itr, end, entry, IsWorldMap);
    itr->second->OnDestroy(map);
    SCR_MAP_END;

    SCR_MAP_BGN(InstanceMapScript, map, itr, end, entry, IsDungeon);
    itr->second->OnDestroy((InstanceMap*)map);
    SCR_MAP_END;

    SCR_MAP_BGN(BattlegroundMapScript, map, itr, end, entry, IsBattleground);
    itr->second->OnDestroy((BattlegroundMap*)map);
    SCR_MAP_END;
}

void ScriptMgr::OnLoadGridMap(Map* map, GridMap* gmap, uint32 gx, uint32 gy)
{
    ASSERT(map);
    ASSERT(gmap);

    SCR_MAP_BGN(WorldMapScript, map, itr, end, entry, IsWorldMap);
    itr->second->OnLoadGridMap(map, gmap, gx, gy);
    SCR_MAP_END;

    SCR_MAP_BGN(InstanceMapScript, map, itr, end, entry, IsDungeon);
    itr->second->OnLoadGridMap((InstanceMap*)map, gmap, gx, gy);
    SCR_MAP_END;

    SCR_MAP_BGN(BattlegroundMapScript, map, itr, end, entry, IsBattleground);
    itr->second->OnLoadGridMap((BattlegroundMap*)map, gmap, gx, gy);
    SCR_MAP_END;
}

void ScriptMgr::OnUnloadGridMap(Map* map, GridMap* gmap, uint32 gx, uint32 gy)
{
    ASSERT(map);
    ASSERT(gmap);

    SCR_MAP_BGN(WorldMapScript, map, itr, end, entry, IsWorldMap);
    itr->second->OnUnloadGridMap(map, gmap, gx, gy);
    SCR_MAP_END;

    SCR_MAP_BGN(InstanceMapScript, map, itr, end, entry, IsDungeon);
    itr->second->OnUnloadGridMap((InstanceMap*)map, gmap, gx, gy);
    SCR_MAP_END;

    SCR_MAP_BGN(BattlegroundMapScript, map, itr, end, entry, IsBattleground);
    itr->second->OnUnloadGridMap((BattlegroundMap*)map, gmap, gx, gy);
    SCR_MAP_END;
}

void ScriptMgr::OnPlayerEnterMap(Map* map, Player* player)
{
    ASSERT(map);
    ASSERT(player);

    //FOREACH_SCRIPT(PlayerScript)->OnMapChanged(player);

    SCR_MAP_BGN(WorldMapScript, map, itr, end, entry, IsWorldMap);
    itr->second->OnPlayerEnter(map, player);
    SCR_MAP_END;

    SCR_MAP_BGN(InstanceMapScript, map, itr, end, entry, IsDungeon);
    itr->second->OnPlayerEnter((InstanceMap*)map, player);
    SCR_MAP_END;

    SCR_MAP_BGN(BattlegroundMapScript, map, itr, end, entry, IsBattleground);
    itr->second->OnPlayerEnter((BattlegroundMap*)map, player);
    SCR_MAP_END;
}

void ScriptMgr::OnPlayerLeaveMap(Map* map, Player* player)
{
    ASSERT(map);
    ASSERT(player);

    SCR_MAP_BGN(WorldMapScript, map, itr, end, entry, IsWorldMap);
    itr->second->OnPlayerLeave(map, player);
    SCR_MAP_END;

    SCR_MAP_BGN(InstanceMapScript, map, itr, end, entry, IsDungeon);
    itr->second->OnPlayerLeave((InstanceMap*)map, player);
    SCR_MAP_END;

    SCR_MAP_BGN(BattlegroundMapScript, map, itr, end, entry, IsBattleground);
    itr->second->OnPlayerLeave((BattlegroundMap*)map, player);
    SCR_MAP_END;
}

void ScriptMgr::OnMapUpdate(Map* map, uint32 diff)
{
    ASSERT(map);

    SCR_MAP_BGN(WorldMapScript, map, itr, end, entry, IsWorldMap);
    itr->second->OnUpdate(map, diff);
    SCR_MAP_END;

    SCR_MAP_BGN(InstanceMapScript, map, itr, end, entry, IsDungeon);
    itr->second->OnUpdate((InstanceMap*)map, diff);
    SCR_MAP_END;

    SCR_MAP_BGN(BattlegroundMapScript, map, itr, end, entry, IsBattleground);
    itr->second->OnUpdate((BattlegroundMap*)map, diff);
    SCR_MAP_END;
}

#undef SCR_MAP_BGN
#undef SCR_MAP_END

void ScriptMgr::OnAddPassenger(Transport* transport, Player* player)
{
    ASSERT(transport);
    ASSERT(player);

   /* GET_SCRIPT(TransportScript, transport->GetScriptId(), tmpscript);
    tmpscript->OnAddPassenger(transport, player); */
}

void ScriptMgr::OnAddCreaturePassenger(Transport* transport, Creature* creature)
{
    ASSERT(transport);
    ASSERT(creature);

    /*GET_SCRIPT(TransportScript, transport->GetScriptId(), tmpscript);
    tmpscript->OnAddCreaturePassenger(transport, creature);*/
}

void ScriptMgr::OnRemovePassenger(Transport* transport, Player* player)
{
    ASSERT(transport);
    ASSERT(player);

    /*GET_SCRIPT(TransportScript, transport->GetScriptId(), tmpscript);
    tmpscript->OnRemovePassenger(transport, player);*/
}

void ScriptMgr::OnTransportUpdate(Transport* transport, uint32 diff)
{
    ASSERT(transport);

    /*GET_SCRIPT(TransportScript, transport->GetScriptId(), tmpscript);
    tmpscript->OnUpdate(transport, diff);*/
}

void ScriptMgr::OnRelocate(Transport* transport, uint32 waypointId, uint32 mapId, float x, float y, float z)
{
/*    GET_SCRIPT(TransportScript, transport->GetScriptId(), tmpscript);
    tmpscript->OnRelocate(transport, waypointId, mapId, x, y, z); */
}

char const* ScriptMgr::ScriptsVersion()
{
    return "Sunstrider scripting library";
}

bool ScriptMgr::OnGossipHello( Player * player, Creature *creature)
{
    ASSERT(player);
    ASSERT(creature);
    
    GET_SCRIPT_RET(CreatureScript, creature->GetScriptId(), tmpscript, false);
    player->PlayerTalkClass->ClearMenus();
    return tmpscript->OnGossipHello(player, creature);
}

//Difference from TC: This clears menu before calling script OnGossipSelect
bool ScriptMgr::OnGossipSelect( Player* player, Creature* creature, uint32 sender, uint32 action )
{
    ASSERT(player);
    ASSERT(creature);

    GET_SCRIPT_RET(CreatureScript, creature->GetScriptId(), tmpscript, false);
    player->PlayerTalkClass->ClearMenus();
    return tmpscript->OnGossipSelect(player, creature, sender, action);
}

//Difference from TC: This clears menu before calling script OnGossipSelectCode
bool ScriptMgr::OnGossipSelectCode( Player *player, Creature *creature, uint32 sender, uint32 action, const char* code )
{
    ASSERT(player);
    ASSERT(creature);
    ASSERT(code);

    GET_SCRIPT_RET(CreatureScript, creature->GetScriptId(), tmpscript, false);
    player->PlayerTalkClass->ClearMenus();
    return tmpscript->OnGossipSelectCode(player, creature, sender, action, code);
}


bool ScriptMgr::OnGossipSelectCode(Player* player, GameObject* go, uint32 sender, uint32 action, const char* code)
{
    ASSERT(player);
    ASSERT(go);
    ASSERT(code);

    GET_SCRIPT_RET(GameObjectScript, go->GetScriptId(), tmpscript, false);
    player->PlayerTalkClass->ClearMenus();
    return tmpscript->OnGossipSelectCode(player, go, sender, action, code);
}

bool ScriptMgr::OnQuestAccept( Player *player, Creature *creature, Quest const *quest)
{
    ASSERT(player);
    ASSERT(creature);
    ASSERT(quest);

    GET_SCRIPT_RET(CreatureScript, creature->GetScriptId(), tmpscript, false);
    player->PlayerTalkClass->ClearMenus();
    return tmpscript->OnQuestAccept(player, creature, quest);

}

bool ScriptMgr::OnQuestSelect( Player *player, Creature *creature, Quest const *quest)
{
    ASSERT(player);
    ASSERT(creature);
    ASSERT(quest);

    GET_SCRIPT_RET(CreatureScript, creature->GetScriptId(), tmpscript, false);
    player->PlayerTalkClass->ClearMenus();
    return tmpscript->OnQuestSelect(player, creature, quest);
}

bool ScriptMgr::OnQuestComplete( Player *player, Creature *creature, Quest const *quest)
{
    ASSERT(player);
    ASSERT(creature);
    ASSERT(quest);

    GET_SCRIPT_RET(CreatureScript, creature->GetScriptId(), tmpscript, false);
    player->PlayerTalkClass->ClearMenus();
    return tmpscript->OnQuestComplete(player, creature, quest);
}

bool ScriptMgr::OnQuestReward( Player *player, Creature *creature, Quest const *quest, uint32 opt )
{
    ASSERT(player);
    ASSERT(creature);
    ASSERT(quest);

    GET_SCRIPT_RET(CreatureScript, creature->GetScriptId(), tmpscript, false);
    player->PlayerTalkClass->ClearMenus();
    return tmpscript->OnQuestReward(player, creature, quest, opt);
}


uint32 ScriptMgr::GetDialogStatus( Player *player, Creature *creature)
{
    ASSERT(player);
    ASSERT(creature);

    GET_SCRIPT_RET(CreatureScript, creature->GetScriptId(), tmpscript, DIALOG_STATUS_SCRIPTED_NO_STATUS);
    player->PlayerTalkClass->ClearMenus();
    return tmpscript->GetDialogStatus(player, creature);
}

CreatureAI* ScriptMgr::GetCreatureAI(Creature* creature)
{
    ASSERT(creature);

    GET_SCRIPT_RET(CreatureScript, creature->GetScriptId(), tmpscript, nullptr);
    return tmpscript->GetAI(creature);
}

void ScriptMgr::OnCreatureUpdate(Creature* creature, uint32 diff)
{
    ASSERT(creature);

    GET_SCRIPT(CreatureScript, creature->GetScriptId(), tmpscript);
    tmpscript->OnUpdate(creature, diff);
}

bool ScriptMgr::OnGossipHello( Player *player, GameObject *go )
{
    ASSERT(player);
    ASSERT(go);

    GET_SCRIPT_RET(GameObjectScript, go->GetScriptId(), tmpscript, false);
    player->PlayerTalkClass->ClearMenus();
    return tmpscript->OnGossipHello(player, go);
}

bool ScriptMgr::OnGossipSelect(Player* player, GameObject* go, uint32 sender, uint32 action)
{
    ASSERT(player);
    ASSERT(go);

    GET_SCRIPT_RET(GameObjectScript, go->GetScriptId(), tmpscript, false);
    return tmpscript->OnGossipSelect(player, go, sender, action);
}

bool ScriptMgr::OnQuestAccept(Player* player, GameObject* go, Quest const* quest)
{
    ASSERT(player);
    ASSERT(go);
    ASSERT(quest);

    GET_SCRIPT_RET(GameObjectScript, go->GetScriptId(), tmpscript, false);
    player->PlayerTalkClass->ClearMenus();
    return tmpscript->OnQuestAccept(player, go, quest);
}

bool ScriptMgr::OnQuestReward(Player* player, GameObject* go, Quest const* quest, uint32 opt)
{
    ASSERT(player);
    ASSERT(go);
    ASSERT(quest);

    GET_SCRIPT_RET(GameObjectScript, go->GetScriptId(), tmpscript, false);
    player->PlayerTalkClass->ClearMenus();
    return tmpscript->OnQuestReward(player, go, quest, opt);
}

uint32 ScriptMgr::GetDialogStatus(Player* player, GameObject* go)
{
    ASSERT(player);
    ASSERT(go);

    GET_SCRIPT_RET(GameObjectScript, go->GetScriptId(), tmpscript, DIALOG_STATUS_SCRIPTED_NO_STATUS);
    player->PlayerTalkClass->ClearMenus();
    return tmpscript->GetDialogStatus(player, go);
}

void ScriptMgr::OnGameObjectDestroyed(GameObject* go, Player* player)
{
    ASSERT(go);

    GET_SCRIPT(GameObjectScript, go->GetScriptId(), tmpscript);
    tmpscript->OnDestroyed(go, player);
}

void ScriptMgr::OnGameObjectDamaged(GameObject* go, Player* player)
{
    ASSERT(go);

    GET_SCRIPT(GameObjectScript, go->GetScriptId(), tmpscript);
    tmpscript->OnDamaged(go, player);
}

void ScriptMgr::OnGameObjectLootStateChanged(GameObject* go, uint32 state, Unit* unit)
{
    ASSERT(go);

    GET_SCRIPT(GameObjectScript, go->GetScriptId(), tmpscript);
    tmpscript->OnLootStateChanged(go, state, unit);
}

void ScriptMgr::OnGameObjectStateChanged(GameObject* go, uint32 state)
{
    ASSERT(go);

    GET_SCRIPT(GameObjectScript, go->GetScriptId(), tmpscript);
    tmpscript->OnGameObjectStateChanged(go, state);
}

void ScriptMgr::OnGameObjectUpdate(GameObject* go, uint32 diff)
{
    ASSERT(go);

    GET_SCRIPT(GameObjectScript, go->GetScriptId(), tmpscript);
    tmpscript->OnUpdate(go, diff);
}

GameObjectAI* ScriptMgr::GetGameObjectAI(GameObject* go)
{
    ASSERT(go);

    GET_SCRIPT_RET(GameObjectScript, go->GetScriptId(), tmpscript, nullptr);
    return tmpscript->GetAI(go);
}

bool ScriptMgr::OnAreaTrigger(Player* player, AreaTriggerEntry const* trigger)
{
    ASSERT(player);
    ASSERT(trigger);

    GET_SCRIPT_RET(AreaTriggerScript, sObjectMgr->GetAreaTriggerScriptId(trigger->id), tmpscript, false);
    return tmpscript->OnTrigger(player, trigger);
}

bool ScriptMgr::OnQuestAccept(Player* player, Item* item, Quest const* quest)
{
    ASSERT(player);
    ASSERT(item);
    ASSERT(quest);

    GET_SCRIPT_RET(ItemScript, item->GetScriptId(), tmpscript, false);
    player->PlayerTalkClass->ClearMenus();
    return tmpscript->OnQuestAccept(player, item, quest);
}

bool ScriptMgr::OnItemUse(Player* player, Item* item, SpellCastTargets const& targets)
{
    ASSERT(player);
    ASSERT(item);

    GET_SCRIPT_RET(ItemScript, item->GetScriptId(), tmpscript, false);
    return tmpscript->OnUse(player, item, targets);
}

bool ScriptMgr::OnItemExpire(Player* player, ItemTemplate const* proto)
{
    ASSERT(player);
    ASSERT(proto);

    GET_SCRIPT_RET(ItemScript, proto->ScriptId, tmpscript, false);
    return tmpscript->OnExpire(player, proto);
}

bool ScriptMgr::ReceiveEmote( Player *player, Creature *creature, uint32 emote )
{
    ASSERT(player);
    ASSERT(creature);

    GET_SCRIPT_RET(CreatureScript, creature->GetScriptId(), tmpscript, DIALOG_STATUS_SCRIPTED_NO_STATUS);
    player->PlayerTalkClass->ClearMenus();
    return tmpscript->OnReceiveEmote(player, creature, emote);
}

bool ScriptMgr::EffectDummyCreature(Unit *caster, uint32 spellId, uint32 effIndex, Creature *creature)
{
    GET_SCRIPT_RET(CreatureScript, creature->GetScriptId(), tmpscript, DIALOG_STATUS_SCRIPTED_NO_STATUS);
    return tmpscript->OnEffectDummyCreature(caster, spellId, effIndex, creature);
}

SpellScriptLoader* ScriptMgr::GetSpellScriptLoader(uint32 scriptId)
{
    return ScriptRegistry<SpellScriptLoader>::Instance()->GetScriptById(scriptId);
}


InstanceScript* ScriptMgr::CreateInstanceScript(InstanceMap* map)
{
    ASSERT(map);

    GET_SCRIPT_RET(InstanceMapScript, map->GetScriptId(), tmpscript, nullptr);
    return tmpscript->GetInstanceScript(map);
}


WorldMapScript::WorldMapScript(const char* name, uint32 mapId)
    : ScriptObject(name), MapScript<Map>(mapId)
{
    GetEntry()->IsDungeon();
    if (GetEntry() && !GetEntry()->IsWorldMap())
        TC_LOG_ERROR("scripts","WorldMapScript for map %u is invalid.", mapId);

    ScriptRegistry<WorldMapScript>::Instance()->AddScript(this);
}

InstanceMapScript::InstanceMapScript(const char* name, uint32 mapId)
    : ScriptObject(name), MapScript<InstanceMap>(mapId)
{
    if (GetEntry() && !GetEntry()->IsDungeon())
        TC_LOG_ERROR("scripts", "InstanceMapScript for map %u is invalid.", mapId);

    ScriptRegistry<InstanceMapScript>::Instance()->AddScript(this);
}

ItemScript::ItemScript(const char* name)
    : ScriptObject(name)
{
    ScriptRegistry<ItemScript>::Instance()->AddScript(this);
}

CreatureScript::CreatureScript(const char* name)
    : ScriptObject(name)
{
    ScriptRegistry<CreatureScript>::Instance()->AddScript(this);
}

CreatureScript::~CreatureScript()
{
    //delete used base OLDScript if any
    delete baseScript;
}

GameObjectScript::GameObjectScript(const char* name)
    : ScriptObject(name)
{
    ScriptRegistry<GameObjectScript>::Instance()->AddScript(this);
}

AreaTriggerScript::AreaTriggerScript(const char* name)
    : ScriptObject(name)
{
    ScriptRegistry<AreaTriggerScript>::Instance()->AddScript(this);
}

SpellScriptLoader::SpellScriptLoader(const char* name)
    : ScriptObject(name)
{
    ScriptRegistry<SpellScriptLoader>::Instance()->AddScript(this);
}

void ScriptMgr::FillSpellSummary()
{
    SpellSummary = new TSpellSummary[sObjectMgr->GetMaxSpellId() + 1];

    SpellInfo const* TempSpell;

    //for (int i=0; i < GetSpellStore()->GetNumRows(); i++ )
    for (auto itr = sObjectMgr->GetSpellStore().begin(); itr != sObjectMgr->GetSpellStore().end(); itr++)
    {
        int i = itr->first;
        SpellSummary[i].Effects = 0;
        SpellSummary[i].Targets = 0;

        TempSpell = sSpellMgr->GetSpellInfo(i);
        //This spell doesn't exist
        if (!TempSpell)
            continue;

        for (const auto & Effect : TempSpell->Effects)
        {
            //Spell targets self
            if (Effect.TargetA.GetTarget() == TARGET_UNIT_CASTER)
                SpellSummary[i].Targets |= 1 << (SELECT_TARGET_SELF - 1);

            //Spell targets a single enemy
            if (Effect.TargetA.GetTarget() == TARGET_UNIT_TARGET_ENEMY ||
                Effect.TargetA.GetTarget() == TARGET_DEST_TARGET_ENEMY)
                SpellSummary[i].Targets |= 1 << (SELECT_TARGET_SINGLE_ENEMY - 1);

            //Spell targets AoE at enemy
            if (Effect.TargetA.GetTarget() == TARGET_UNIT_SRC_AREA_ENEMY ||
                Effect.TargetA.GetTarget() == TARGET_UNIT_DEST_AREA_ENEMY ||
                Effect.TargetA.GetTarget() == TARGET_SRC_CASTER ||
                Effect.TargetA.GetTarget() == TARGET_DEST_DYNOBJ_ENEMY)
                SpellSummary[i].Targets |= 1 << (SELECT_TARGET_AOE_ENEMY - 1);

            //Spell targets an enemy
            if (Effect.TargetA.GetTarget() == TARGET_UNIT_TARGET_ENEMY ||
                Effect.TargetA.GetTarget() == TARGET_DEST_TARGET_ENEMY ||
                Effect.TargetA.GetTarget() == TARGET_UNIT_SRC_AREA_ENEMY ||
                Effect.TargetA.GetTarget() == TARGET_UNIT_DEST_AREA_ENEMY ||
                Effect.TargetA.GetTarget() == TARGET_SRC_CASTER ||
                Effect.TargetA.GetTarget() == TARGET_DEST_DYNOBJ_ENEMY)
                SpellSummary[i].Targets |= 1 << (SELECT_TARGET_ANY_ENEMY - 1);

            //Spell targets a single friend(or self)
            if (Effect.TargetA.GetTarget() == TARGET_UNIT_CASTER ||
                Effect.TargetA.GetTarget() == TARGET_UNIT_TARGET_ALLY ||
                Effect.TargetA.GetTarget() == TARGET_UNIT_TARGET_PARTY)
                SpellSummary[i].Targets |= 1 << (SELECT_TARGET_SINGLE_FRIEND - 1);

            //Spell targets aoe friends
            if (Effect.TargetA.GetTarget() == TARGET_UNIT_CASTER_AREA_PARTY ||
                Effect.TargetA.GetTarget() == TARGET_UNIT_LASTTARGET_AREA_PARTY ||
                Effect.TargetA.GetTarget() == TARGET_SRC_CASTER)
                SpellSummary[i].Targets |= 1 << (SELECT_TARGET_AOE_FRIEND - 1);

            //Spell targets any friend(or self)
            if (Effect.TargetA.GetTarget() == TARGET_UNIT_CASTER ||
                Effect.TargetA.GetTarget() == TARGET_UNIT_TARGET_ALLY ||
                Effect.TargetA.GetTarget() == TARGET_UNIT_TARGET_PARTY ||
                Effect.TargetA.GetTarget() == TARGET_UNIT_CASTER_AREA_PARTY ||
                Effect.TargetA.GetTarget() == TARGET_UNIT_LASTTARGET_AREA_PARTY ||
                Effect.TargetA.GetTarget() == TARGET_SRC_CASTER)
                SpellSummary[i].Targets |= 1 << (SELECT_TARGET_ANY_FRIEND - 1);

            //Make sure that this spell includes a damage effect
            if (Effect.Effect == SPELL_EFFECT_SCHOOL_DAMAGE ||
                Effect.Effect == SPELL_EFFECT_INSTAKILL ||
                Effect.Effect == SPELL_EFFECT_ENVIRONMENTAL_DAMAGE ||
                Effect.Effect == SPELL_EFFECT_HEALTH_LEECH)
                SpellSummary[i].Effects |= 1 << (SELECT_EFFECT_DAMAGE - 1);

            //Make sure that this spell includes a healing effect (or an apply aura with a periodic heal)
            if (Effect.Effect == SPELL_EFFECT_HEAL ||
                Effect.Effect == SPELL_EFFECT_HEAL_MAX_HEALTH ||
                Effect.Effect == SPELL_EFFECT_HEAL_MECHANICAL ||
                (Effect.Effect == SPELL_EFFECT_APPLY_AURA  && Effect.ApplyAuraName == 8))
                SpellSummary[i].Effects |= 1 << (SELECT_EFFECT_HEALING - 1);

            //Make sure that this spell applies an aura
            if (Effect.Effect == SPELL_EFFECT_APPLY_AURA)
                SpellSummary[i].Effects |= 1 << (SELECT_EFFECT_AURA - 1);
        }
    }
}

// Specialize for each script type class like so:
template class ScriptRegistry<SpellScriptLoader>;
/*
template class ScriptRegistry<ServerScript>;
template class ScriptRegistry<WorldScript>;
template class ScriptRegistry<FormulaScript>;
*/
template class ScriptRegistry<WorldMapScript>;
template class ScriptRegistry<InstanceMapScript>;
template class ScriptRegistry<BattlegroundMapScript>;
template class ScriptRegistry<ItemScript>;
template class ScriptRegistry<CreatureScript>;
template class ScriptRegistry<GameObjectScript>;
template class ScriptRegistry<AreaTriggerScript>;
/*
template class ScriptRegistry<BattlegroundScript>;
template class ScriptRegistry<OutdoorPvPScript>;
template class ScriptRegistry<CommandScript>;
template class ScriptRegistry<WeatherScript>;
template class ScriptRegistry<AuctionHouseScript>;
template class ScriptRegistry<ConditionScript>;
template class ScriptRegistry<VehicleScript>;
template class ScriptRegistry<DynamicObjectScript>;
template class ScriptRegistry<TransportScript>;
template class ScriptRegistry<AchievementCriteriaScript>;
template class ScriptRegistry<PlayerScript>;
template class ScriptRegistry<GuildScript>;
template class ScriptRegistry<GroupScript>;
*/

// Undefine utility macros.
#undef GET_SCRIPT_RET
#undef GET_SCRIPT
#undef FOREACH_SCRIPT
#undef FOR_SCRIPTS_RET
#undef FOR_SCRIPTS
#undef SCR_REG_LST
#undef SCR_REG_ITR
#undef SCR_REG_MAP