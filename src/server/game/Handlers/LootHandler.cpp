/*
 * Copyright (C) 2008-2015 TrinityCore <http://www.trinitycore.org/>
 * Copyright (C) 2005-2009 MaNGOS <http://getmangos.com/>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "Common.h"
#include "Log.h"
#include "Corpse.h"
#include "Creature.h"
#include "GameObject.h"
#include "Group.h"
#include "GuildMgr.h"
#include "LootMgr.h"
#include "ObjectAccessor.h"
#include "Object.h"
#include "Opcodes.h"
#include "Player.h"
#include "World.h"
#include "WorldPacket.h"
#include "LootPackets.h"
#include "WorldSession.h"
#include "Cell.h"
#include "CellImpl.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"

void WorldSession::HandleAutostoreLootItemOpcode(WorldPackets::Loot::AutoStoreLootItem& packet)
{
    TC_LOG_DEBUG("network", "WORLD: CMSG_AUTOSTORE_LOOT_ITEM");

    for (WorldPackets::Loot::LootRequest const& req : packet.Loot)
    {
        Loot* loot = _player->GetLootObject(req.Object);

        if (!loot)
        {
            TC_LOG_ERROR("network", "WorldSession::HandleAutostoreLootItemOpcode: Player %s tried to loot item from an object which is not being looted.", _player->GetName().c_str());
            continue;
        }

        Object* owner = loot->GetOwner();

        switch (owner->GetTypeId())
        {
            case TYPEID_GAMEOBJECT:
            {
                GameObject* go = owner->ToGameObject();

                // not check distance for GO in case owned GO (fishing bobber case, for example) or Fishing hole GO
                if ((go->GetOwnerGUID() != _player->GetGUID() && go->GetGoType() != GAMEOBJECT_TYPE_FISHINGHOLE) && !go->IsWithinDistInMap(_player, INTERACTION_DISTANCE))
                {
                    _player->SendLootRelease(owner->GetGUID());
                    continue;
                }

                break;
            }
            case TYPEID_UNIT:
            {
                Creature* creature = owner->ToCreature();

                bool lootAllowed = creature->IsAlive() == (_player->getClass() == CLASS_ROGUE && loot->Type == LOOT_PICKPOCKETING);
                if (!lootAllowed || !creature->IsWithinDistInMap(_player, INTERACTION_DISTANCE))
                {
                    _player->SendLootError(owner->GetGUID(), lootAllowed ? LOOT_ERROR_TOO_FAR : LOOT_ERROR_DIDNT_KILL);
                    continue;
                }

                break;
            }
            default:
                break;
        }

        // Since 6.x client sends loot starting from 1 hence the -1
        _player->StoreLootItem(req.LootListID-1, loot);

        // If player is removing the last LootItem, delete the empty container.
        if (loot->IsLooted() && owner->GetTypeId() == TYPEID_ITEM)
            _player->GetSession()->DoLootRelease(loot);
    }
}

void WorldSession::HandleLootMoneyOpcode(WorldPackets::Loot::LootMoney& /*packet*/)
{
    TC_LOG_DEBUG("network", "WORLD: CMSG_LOOT_MONEY");

    /*ObjectGuid guid = _player->GetLootGUID();
    if (!guid)
        return;

    Loot* loot = NULL;
    bool shareMoney = true;

    switch (guid.GetHigh())
    {
        case HighGuid::GameObject:
        {
            GameObject* go = _player->GetMap()->GetGameObject(guid);

            // do not check distance for GO if player is the owner of it (ex. fishing bobber)
            if (go && ((go->GetOwnerGUID() == _player->GetGUID() || go->IsWithinDistInMap(_player, INTERACTION_DISTANCE))))
                loot = &go->loot;

            break;
        }
        case HighGuid::Corpse:                               // remove insignia ONLY in BG
        {
            Corpse* bones = ObjectAccessor::GetCorpse(*_player, guid);

            if (bones && bones->IsWithinDistInMap(_player, INTERACTION_DISTANCE))
            {
                loot = &bones->loot;
                shareMoney = false;
            }

            break;
        }
        case HighGuid::Item:
        {
            if (Item* item = _player->GetItemByGuid(guid))
            {
                loot = &item->loot;
                shareMoney = false;
            }
            break;
        }
        case HighGuid::Creature:
        case HighGuid::Vehicle:
        {
            Creature* creature = _player->GetMap()->GetCreature(guid);
            bool lootAllowed = creature && creature->IsAlive() == (_player->getClass() == CLASS_ROGUE && creature->loot.loot_type == LOOT_PICKPOCKETING);
            if (lootAllowed && creature->IsWithinDistInMap(_player, INTERACTION_DISTANCE))
            {
                loot = &creature->loot;
                if (creature->IsAlive())
                    shareMoney = false;
            }
            else
                _player->SendLootError(guid, lootAllowed ? LOOT_ERROR_TOO_FAR : LOOT_ERROR_DIDNT_KILL);
            break;
        }
        default:
            return;                                         // unlootable type
    }

    if (loot)
    {
        loot->NotifyMoneyRemoved();
        if (shareMoney && _player->GetGroup())      //item, pickpocket and players can be looted only single player
        {
            Group* group = _player->GetGroup();

            std::vector<Player*> playersNear;
            for (GroupReference* itr = group->GetFirstMember(); itr != NULL; itr = itr->next())
            {
                Player* member = itr->GetSource();
                if (!member)
                    continue;

                if (_player->IsWithinDistInMap(member, sWorld->getFloatConfig(CONFIG_GROUP_XP_DISTANCE), false))
                    playersNear.push_back(member);
            }

            uint32 goldPerPlayer = uint32((loot->gold) / (playersNear.size()));

            for (std::vector<Player*>::const_iterator i = playersNear.begin(); i != playersNear.end(); ++i)
            {
                (*i)->ModifyMoney(goldPerPlayer);
                (*i)->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_LOOT_MONEY, goldPerPlayer);

                if (Guild* guild = sGuildMgr->GetGuildById((*i)->GetGuildId()))
                    if (uint32 guildGold = CalculatePct(goldPerPlayer, (*i)->GetTotalAuraModifier(SPELL_AURA_DEPOSIT_BONUS_MONEY_IN_GUILD_BANK_ON_LOOT)))
                        guild->HandleMemberDepositMoney(this, guildGold, true);

                WorldPackets::Loot::LootMoneyNotify packet;
                packet.Money = goldPerPlayer;
                packet.SoleLooter = playersNear.empty();
                (*i)->SendDirectMessage(packet.Write());
            }
        }
        else
        {
            _player->ModifyMoney(loot->gold);
            _player->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_LOOT_MONEY, loot->gold);

            if (Guild* guild = sGuildMgr->GetGuildById(_player->GetGuildId()))
                if (uint32 guildGold = CalculatePct(loot->gold, _player->GetTotalAuraModifier(SPELL_AURA_DEPOSIT_BONUS_MONEY_IN_GUILD_BANK_ON_LOOT)))
                    guild->HandleMemberDepositMoney(this, guildGold, true);

            WorldPackets::Loot::LootMoneyNotify packet;
            packet.Money = loot->gold;
            packet.SoleLooter = true; // "You loot..."
            SendPacket(packet.Write());
        }

        loot->gold = 0;

        // Delete the money loot record from the DB
        if (!loot->containerID.IsEmpty())
            loot->DeleteLootMoneyFromContainerItemDB();

        // Delete container if empty
        if (loot->isLooted() && guid.IsItem())
            _player->GetSession()->DoLootRelease(guid);
    }*/
}

void WorldSession::HandleLootOpcode(WorldPackets::Loot::LootUnit& packet)
{
    TC_LOG_DEBUG("network", "WORLD: CMSG_LOOT");

    // Check possible cheat
    if (!_player->IsAlive() || !packet.Unit.IsCreatureOrVehicle())
        return;

    Creature const* mainCreature = sObjectAccessor->GetCreatureOrPetOrVehicle(*_player, packet.Unit);

    if (!mainCreature || !mainCreature->IsWithinDistInMap(_player, INTERACTION_DISTANCE))
        return;

    // AoE looting searcher
    float aoeDist = sWorld->getFloatConfig(CONFIG_MAX_AOE_LOOT_DISTANCE);
    std::list<Creature*> lootCreatures;
    Trinity::AllLootableCreaturesInRange check(mainCreature, aoeDist);
    Trinity::CreatureListSearcher<Trinity::AllLootableCreaturesInRange> searcher(mainCreature, lootCreatures, check);
    mainCreature->VisitNearbyObject(aoeDist, searcher);

    // Send number of AoE loot targets (main target is also included in the search)
    WorldPackets::Loot::AELootTargets targetsPacket;
    targetsPacket.Count = lootCreatures.size();
    SendPacket(targetsPacket.Write());

    // Send loot of main creature
    //GetPlayer()->SendLoot(mainCreature, LOOT_CORPSE);

    // SMSG_AE_LOOT_TARGET_ACK
    WorldPackets::Loot::AELootTargetAck targetAckPacket;
    WorldPacket const* targetAckData = targetAckPacket.Write();
    //SendPacket(targetAckData);

    // Send loot of AoE targets
    for (Creature const* creature : lootCreatures)
    {
        if (creature->IsAlive() || !creature->loot || !(creature->loot->Type == LOOT_CORPSE || creature->loot->Type == LOOT_SKINNING))
            return;

        _player->SendLoot(creature->loot, creature != mainCreature);

        // SMSG_AE_LOOT_TARGET_ACK is sent after every loot response
        SendPacket(targetAckData);
    }

    // interrupt cast
    if (_player->IsNonMeleeSpellCast(false))
        _player->InterruptNonMeleeSpells(false);
}

void WorldSession::HandleLootReleaseOpcode(WorldPackets::Loot::LootRelease& packet)
{
    TC_LOG_DEBUG("network", "WORLD: CMSG_LOOT_RELEASE");

    // cheaters can modify lguid to prevent correct apply loot release code and re-loot
    // use internal stored guid
    PlayerLootObjects& lootObjects = _player->GetLootObjects();
    if (lootObjects.empty())
        return;

    for (auto& kv : lootObjects)
    {
        if (kv.second->GetOwner()->GetGUID() == packet.Unit)
        {
            DoLootRelease(kv.second);
            return;
        }
    }
}

void WorldSession::DoLootRelease(Loot* loot)
{
    Player* player = GetPlayer();

    player->SendLootRelease(loot->GetOwner()->GetGUID());

    player->RemoveFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_LOOTING);

    if (!player->IsInWorld())
        return;

    //Player is not looking at loot list, he doesn't need to see updates on the loot list
    loot->RemoveLooter(player->GetGUID());
    player->RemoveLootObject(loot->GetGUID());

    Object* owner = loot->GetOwner();

    switch (owner->GetTypeId())
    {
        case TYPEID_GAMEOBJECT:
        {
            GameObject* go = owner->ToGameObject();

            // not check distance for GO in case owned GO (fishing bobber case, for example) or Fishing hole GO
            if ((go->GetOwnerGUID() != _player->GetGUID() && go->GetGoType() != GAMEOBJECT_TYPE_FISHINGHOLE) && !go->IsWithinDistInMap(_player, INTERACTION_DISTANCE))
                return;

            if (go->GetGoType() == GAMEOBJECT_TYPE_DOOR)
            {
                // locked doors are opened with spelleffect openlock, prevent remove its as looted
                go->UseDoorOrButton();
            }
            else if (loot->IsLooted() || go->GetGoType() == GAMEOBJECT_TYPE_FISHINGNODE)
            {
                if (go->GetGoType() == GAMEOBJECT_TYPE_FISHINGHOLE)
                {                                               // The fishing hole used once more
                    go->AddUse();                               // if the max usage is reached, will be despawned in next tick
                    if (go->GetUseCount() >= go->GetGOValue()->FishingHole.MaxOpens)
                        go->SetLootState(GO_JUST_DEACTIVATED);
                    else
                        go->SetLootState(GO_READY);
                }
                else
                    go->SetLootState(GO_JUST_DEACTIVATED);

                delete loot;
                go->loot = nullptr;
                loot = nullptr;
            }
            else
            {
                // not fully looted object
                go->SetLootState(GO_ACTIVATED, player);

                // if the round robin player release, reset it.
                if (player->GetGUID() == loot->RoundRobinPlayer)
                    loot->RoundRobinPlayer.Clear();
            }

            break;
        }
        case TYPEID_CORPSE:
        {
            Corpse* corpse = owner->ToCorpse();

            if (!corpse->IsWithinDistInMap(_player, INTERACTION_DISTANCE))
                return;

            if (loot->IsLooted())
                corpse->RemoveFlag(CORPSE_FIELD_DYNAMIC_FLAGS, CORPSE_DYNFLAG_LOOTABLE);

            break;
        }
        case TYPEID_ITEM:
        {
            Item* pItem = player->GetItemByGuid(owner->GetGUID());

            if (!pItem)
                return;

            ItemTemplate const* proto = pItem->GetTemplate();

            // destroy only 5 items from stack in case prospecting and milling
            if (proto->GetFlags() & (ITEM_PROTO_FLAG_PROSPECTABLE | ITEM_PROTO_FLAG_MILLABLE))
            {
                delete loot;
                pItem->loot = nullptr;
                loot = nullptr;

                uint32 count = pItem->GetCount();

                // >=5 checked in spell code, but will work for cheating cases also with removing from another stacks.
                if (count > 5)
                    count = 5;

                player->DestroyItemCount(pItem, count, true);
            }
            else
            {
                // Only delete item if no loot or money (unlooted loot is saved to db) or if it isn't an openable item
                if (pItem->loot->IsLooted() || !(proto->GetFlags() & ITEM_PROTO_FLAG_OPENABLE))
                    player->DestroyItem(pItem->GetBagSlot(), pItem->GetSlot(), true);
            }

            break; 
        }
        case TYPEID_UNIT:
        {
            Creature* creature = owner->ToCreature();

            bool lootAllowed = creature->IsAlive() == (player->getClass() == CLASS_ROGUE && loot->Type == LOOT_PICKPOCKETING);
            if (!lootAllowed || !creature->IsWithinDistInMap(player, INTERACTION_DISTANCE))
                return;

            if (loot->IsLooted())
            {
                creature->RemoveFlag(OBJECT_DYNAMIC_FLAGS, UNIT_DYNFLAG_LOOTABLE);

                // skip pickpocketing loot for speed, skinning timer reduction is no-op in fact
                if (!creature->IsAlive())
                    creature->AllLootRemovedFromCorpse();
            }
            else
            {
                // if the round robin player release, reset it.
                if (player->GetGUID() == loot->RoundRobinPlayer)
                {
                    loot->RoundRobinPlayer.Clear();

                    if (Group* group = player->GetGroup())
                    {
                        group->SendLooter(creature, NULL);

                        // force update of dynamic flags, otherwise other group's players still not able to loot.
                        creature->ForceValuesUpdateAtIndex(OBJECT_DYNAMIC_FLAGS);
                    }
                }
            }

            break;
        }
        default:
            break;
    }
}

void WorldSession::HandleLootMasterGiveOpcode(WorldPacket& recvData)
{
    uint8 slotid;
    ObjectGuid lootguid, target_playerguid;

    recvData >> lootguid >> slotid >> target_playerguid;

    if (!_player->GetGroup() || _player->GetGroup()->GetMasterLooterGuid() != _player->GetGUID() || _player->GetGroup()->GetLootMethod() != MASTER_LOOT)
    {
        _player->SendLootError(lootguid, LOOT_ERROR_DIDNT_KILL);
        return;
    }

    Player* target = ObjectAccessor::FindPlayer(target_playerguid);
    if (!target)
    {
        _player->SendLootError(lootguid, LOOT_ERROR_PLAYER_NOT_FOUND);
        return;
    }

    TC_LOG_DEBUG("network", "WorldSession::HandleLootMasterGiveOpcode (CMSG_LOOT_MASTER_GIVE, 0x02A3) Target = [%s].", target->GetName().c_str());

    if (!_player->IsInRaidWith(target) || !_player->IsInMap(target))
    {
        _player->SendLootError(lootguid, LOOT_ERROR_MASTER_OTHER);
        TC_LOG_INFO("loot", "MasterLootItem: Player %s tried to give an item to ineligible player %s !", GetPlayer()->GetName().c_str(), target->GetName().c_str());
        return;
    }

    Loot* loot = NULL;

    /** 6.x @todo loot update
    if (GetPlayer()->GetLootGUID().IsCreatureOrVehicle())
    {
        Creature* creature = GetPlayer()->GetMap()->GetCreature(lootguid);
        if (!creature)
            return;

        loot = &creature->loot;
    }
    else if (GetPlayer()->GetLootGUID().IsGameObject())
    {
        GameObject* pGO = GetPlayer()->GetMap()->GetGameObject(lootguid);
        if (!pGO)
            return;

        loot = &pGO->loot;
    }**/

    if (!loot)
    {
        _player->SendLootError(lootguid, LOOT_ERROR_DIDNT_KILL);
        return;
    }

    if (slotid >= loot->Items.size() + loot->QuestItems.size())
    {
        TC_LOG_DEBUG("loot", "MasterLootItem: Player %s might be using a hack! (slot %d, size %lu)",
            GetPlayer()->GetName().c_str(), slotid, (unsigned long)loot->Items.size());
        return;
    }

    LootItem& item = slotid >= loot->Items.size() ? loot->QuestItems[slotid - loot->Items.size()] : loot->Items[slotid];

    ItemPosCountVec dest;
    InventoryResult msg = target->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, item.itemid, item.count);
    if (item.follow_loot_rules && !item.AllowedForPlayer(target))
        msg = EQUIP_ERR_CANT_EQUIP_EVER;
    if (msg != EQUIP_ERR_OK)
    {
        if (msg == EQUIP_ERR_ITEM_MAX_COUNT)
            _player->SendLootError(lootguid, LOOT_ERROR_MASTER_UNIQUE_ITEM);
        else if (msg == EQUIP_ERR_INV_FULL)
            _player->SendLootError(lootguid, LOOT_ERROR_MASTER_INV_FULL);
        else
            _player->SendLootError(lootguid, LOOT_ERROR_MASTER_OTHER);

        target->SendEquipError(msg, NULL, NULL, item.itemid);
        return;
    }

    // now move item from loot to target inventory
    Item* newitem = target->StoreNewItem(dest, item.itemid, true, item.randomPropertyId, item.GetAllowedLooters());
    target->SendNewItem(newitem, uint32(item.count), false, false, true);
    target->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_LOOT_ITEM, item.itemid, item.count);
    target->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_LOOT_TYPE, item.itemid, item.count, loot->Type);
    target->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_LOOT_EPIC_ITEM, item.itemid, item.count);

    // mark as looted
    item.count = 0;
    item.is_looted = true;

    loot->NotifyItemRemoved(slotid);
    --loot->UnlootedCount;
}
