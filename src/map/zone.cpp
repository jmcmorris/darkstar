﻿/*
===========================================================================

  Copyright (c) 2010-2012 Darkstar Dev Teams

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see http://www.gnu.org/licenses/

  This file is part of DarkStar-server source code.

===========================================================================
*/

#include "../common/showmsg.h"
#include "../common/timer.h"
#include "../common/utils.h"

#include <string.h>

#include "battleutils.h"
#include "charutils.h"
#include "enmity_container.h"
#include "itemutils.h"
#include "map.h"
#include "mobutils.h"
#include "npcentity.h"
#include "petentity.h"
#include "spell.h"
#include "treasure_pool.h"
#include "vana_time.h"
#include "zone.h"

#include "lua/luautils.h"

#include "packets/action.h"
#include "packets/char.h"
#include "packets/char_update.h"
#include "packets/entity_update.h"
#include "packets/fade_out.h"
#include "packets/inventory_assign.h"
#include "packets/inventory_finish.h"
#include "packets/inventory_item.h"
#include "packets/lock_on.h"
#include "packets/message_basic.h"
#include "packets/server_ip.h"
#include "packets/wide_scan.h"


/************************************************************************
*																		*
*  Cервер для обработки активности сущностей (по серверу на зону) без	*
*  активных областей													*	 
*																		*
************************************************************************/

int32 zone_server(uint32 tick, CTaskMgr::CTask* PTask)
{
	((CZone*)PTask->m_data)->ZoneServer(tick);
	return 0;
}

/************************************************************************
*																		*
*  Cервер для обработки активности сущностей (по серверу на зону) c		*
*  активными областями													*	 
*																		*
************************************************************************/

int32 zone_server_region(uint32 tick, CTaskMgr::CTask* PTask)
{
	CZone* PZone = (CZone*)PTask->m_data;
	
	if ((tick - PZone->m_RegionCheckTime) < 2000)
	{
		PZone->ZoneServer(tick);
	} else {
		PZone->ZoneServerRegion(tick);
		PZone->m_RegionCheckTime = tick;
	}
	return 0;
}

/************************************************************************
*																		*
*  Класс CZone															*	 
*																		*
************************************************************************/

CZone::CZone(uint8 ZoneID, uint8 RegionID)
{
	ZoneTimer = NULL;

	m_zoneID = ZoneID;
    m_regionID = RegionID;
    m_Transport = 0;
	m_TreasurePool = 0;
	m_RegionCheckTime = 0;
	m_InstanceHandler = NULL;

	switch(m_zoneID){ //all bcnm zones
		case 139:
		case 146:
		case 206:
		case 144:
		case 168:
		case 163:
		case 141:
			m_InstanceHandler = new CInstanceHandler(m_zoneID);
	}

	LoadZoneLines();
	LoadZoneSettings();
}

/************************************************************************
*																		*
*  Деструктор															*
*																		*
*  На самом деле в удалении элементов зоны нет необходимости, так как	*
*  данный класс уничтожается лишь при завершении работы сервера			*	 
*																		*
************************************************************************/

CZone::~CZone()
{
	while(!m_zoneLineList.empty())
	{
		delete m_zoneLineList.front();
		m_zoneLineList.pop_front();
	}
	while(!m_regionList.empty())
	{
		delete m_regionList.front();
		m_regionList.pop_front();
	}
	while(!m_npcList.empty())
	{
		delete m_npcList.begin()->second;
		m_npcList.erase(m_npcList.begin());
	}
	while(!m_mobList.empty())
	{
		delete m_mobList.begin()->second;
		m_mobList.erase(m_mobList.begin());
	}
	while(!m_petList.empty())
	{
		delete m_petList.begin()->second;
		m_petList.erase(m_petList.begin());
	}
    delete m_Transport;
	delete m_TreasurePool;
}

uint8 CZone::GetID()
{
	return m_zoneID;
}

uint8 CZone::GetRegionID()
{
    return m_regionID;
}

uint32 CZone::GetIP()
{
	return m_zoneIP;
}

uint16 CZone::GetPort()
{
	return m_zonePort;
}

uint16 CZone::GetTax()
{
	return m_tax;
}

const int8* CZone::GetName()
{
	return m_zoneName.c_str();
}

uint8 CZone::GetSoloBattleMusic()
{
	return m_zoneMusic.m_bSongS;
}

uint8 CZone::GetPartyBattleMusic()
{
	return m_zoneMusic.m_bSongM;
}

uint8 CZone::GetBackgroundMusic()
{
	return m_zoneMusic.m_song;
}

bool CZone::CanUseMisc(uint16 misc)
{
	return (m_miscMask & misc) == misc;
}

zoneLine_t* CZone::GetZoneLine(uint32 zoneLineID)
{	
	for(zoneLineList_t::const_iterator  i = m_zoneLineList.begin();
		i != m_zoneLineList.end(); 
		i++ ) 
	{
		if(	(*i)->m_zoneLineID == zoneLineID ) 
		{
			return (*i);
		}
	}
	return NULL;
}

/************************************************************************
*																		*
*  Загружаем ZoneLines, необходимые для правильного перемещения	между	*
*  зонами.																*	 
*																		*
************************************************************************/

void CZone::LoadZoneLines() 
{	
	const int8* fmtQuery = "SELECT zoneline, tozone, tox, toy, toz, rotation FROM zonelines WHERE fromzone = %u";
					  
	int32 ret = Sql_Query(SqlHandle, fmtQuery, m_zoneID);

	if( ret != SQL_ERROR && Sql_NumRows(SqlHandle) != 0)
	{
		while(Sql_NextRow(SqlHandle) == SQL_SUCCESS) 
		{
			zoneLine_t* zl = new zoneLine_t;

			zl->m_zoneLineID = (uint32)Sql_GetIntData(SqlHandle,0);
			zl->m_toZone  = (uint8)Sql_GetIntData(SqlHandle,1);
			zl->m_toPos.x = Sql_GetFloatData(SqlHandle,2);
			zl->m_toPos.y = Sql_GetFloatData(SqlHandle,3);
			zl->m_toPos.z = Sql_GetFloatData(SqlHandle,4);
			zl->m_toPos.rotation = (uint8)Sql_GetIntData(SqlHandle,5);

			m_zoneLineList.push_back(zl);
		}
	}
}

/************************************************************************
*																		*
*  Загружаем настройки зоны из базы										*	 
*																		*
************************************************************************/

void CZone::LoadZoneSettings() 
{
	const int8* fmtQuery = "SELECT name, zoneip, zoneport, music, battlesolo, battlemulti, tax, misc \
						    FROM zone_settings \
							WHERE zoneid = %u \
							LIMIT 1";
					  
	int32 ret = Sql_Query(SqlHandle, fmtQuery, m_zoneID);

	if (ret == SQL_ERROR || 
		Sql_NumRows(SqlHandle) == 0 ||
		Sql_NextRow(SqlHandle) != SQL_SUCCESS) 
	{
		ShowFatalError(CL_RED"CZone::LoadZoneSettings: Cannot loading zone settings (%u)\n" CL_RESET, m_zoneID);
	} 
	else 
	{
		int8* tmpName;
		Sql_GetData(SqlHandle,0,&tmpName,NULL);
		m_zoneName.insert(0,tmpName);

		m_zoneIP   = (uint32)Sql_GetUIntData(SqlHandle,1);
		m_zonePort = (uint16)Sql_GetUIntData(SqlHandle,2);
		m_zoneMusic.m_song   = (uint8)Sql_GetUIntData(SqlHandle,3);		// background music
		m_zoneMusic.m_bSongS = (uint8)Sql_GetUIntData(SqlHandle,4);		// solo battle music
		m_zoneMusic.m_bSongM = (uint8)Sql_GetUIntData(SqlHandle,5);		// party battle music
		m_tax = (uint16)(Sql_GetFloatData(SqlHandle,6) * 100);			// tax for bazaar
		m_miscMask = (uint16)Sql_GetUIntData(SqlHandle,7);

		if (m_miscMask & MISC_TREASURE)
		{
			m_TreasurePool = new CTreasurePool(TREASUREPOOL_ZONE);
		}
	}
}

/***********************************************************************
		Loads the zones BCNM instances from the database
************************************************************************
void CZone::LoadZoneInstances() 
{
	const int8* fmtQuery = "SELECT name, bcnmId, fastestName, fastestTime, timeLimit, levelCap, lootDropId, rules, partySize \
						    FROM bcnm_info \
							WHERE zoneId = %u ";
					  
	int32 ret = Sql_Query(SqlHandle, fmtQuery, m_zoneID);

	if (ret == SQL_ERROR || 
		Sql_NumRows(SqlHandle) == 0) 
	{
		switch(m_zoneID){
		case 139:
		case 146:
		case 206:
		case 144:
		case 168:
			ShowError(CL_RED"CZone::LoadZoneInstances: Cannot load zone BCNM instances (%u)\n" CL_RESET, m_zoneID);
		}
	} 
	else 
	{
		CInstanceHandler* PInstHand = new CInstanceHandler(m_zoneID);

		while(Sql_NextRow(SqlHandle) == SQL_SUCCESS){
			uint8 instance = 1;
			while(instance<=3){ //3 instances for everything as far as I know
				CInstance* PInstance = new CInstance(Sql_GetUIntData(SqlHandle,1));
			
				int8* tmpName;
				Sql_GetData(SqlHandle,0,&tmpName,NULL);
				PInstance->setBcnmName(tmpName);

				PInstance->setZoneId(m_zoneID);
				PInstance->setTimeLimit(Sql_GetUIntData(SqlHandle,4));
				PInstance->setLevelCap(Sql_GetUIntData(SqlHandle,5));
				PInstance->setDropId(Sql_GetUIntData(SqlHandle,6));
				PInstance->setMaxParticipants(Sql_GetUIntData(SqlHandle,8));
				PInstance->setInstanceNumber(instance);
				PInstance->m_RuleMask = (uint16)Sql_GetUIntData(SqlHandle,7);

				PInstHand->storeInstance(PInstance);
				instance++;
			}

			//ShowDebug("Added %s \n",tmpName);
		}
		m_InstanceHandler = PInstHand;
	}
}*/


/************************************************************************
*																		*
*  Добавляем в зону MOB													*
*																		*
************************************************************************/

void CZone::InsertMOB(CBaseEntity* PMob)
{
	if ((PMob != NULL) && (PMob->objtype == TYPE_MOB))
	{
        PMob->loc.zone = this;

        FindPartyForMob(PMob);
		m_mobList[PMob->targid] = PMob;
	}
}

/************************************************************************
*																		*
*  Добавляем в зону NPC													*	 
*																		*
************************************************************************/

void CZone::InsertNPC(CBaseEntity* PNpc)
{
	if ((PNpc != NULL) && (PNpc->objtype == TYPE_NPC))
	{
        PNpc->loc.zone = this;

        if (PNpc->look.size == MODEL_SHIP)
        {
            m_Transport = PNpc;
            return;
        }
		m_npcList[PNpc->targid] = PNpc;
	}
}

void CZone::DeletePET(CBaseEntity* PPet){
	if(PPet!=NULL){
		m_petList.erase(PPet->targid);
	}
}

/************************************************************************
*                                                                       *
*  Добавляем в зону PET (свободные targid 0x700-0x7FF)                  *	 
*                                                                       *
************************************************************************/

void CZone::InsertPET(CBaseEntity* PPet)
{
	if ((PPet != NULL) && (PPet->objtype == TYPE_PET))
	{
        uint16 targid = 0x700;

        for (EntityList_t::const_iterator it = m_petList.begin() ; it != m_petList.end() ; ++it)
	    {
            if (targid != it->first)
            {
                break;
            }
		    targid++;
	    }
        if (targid >= 0x800)
        {
            ShowError(CL_RED"CZone::InsertPET : targid is high (03hX)\n" CL_RESET, targid);
            return;
        }
        PPet->id = 0x1000000 + (m_zoneID << 12) + targid;
        PPet->targid = targid;
        PPet->loc.zone = this;
		m_petList[PPet->targid] = PPet;

		for (EntityList_t::const_iterator it = m_charList.begin() ; it != m_charList.end() ; ++it)
		{
			CCharEntity* PCurrentChar = (CCharEntity*)it->second;

			if(distance(PPet->loc.p, PCurrentChar->loc.p) < 50) 
			{
				PCurrentChar->SpawnPETList[PPet->id] = PPet;
				PCurrentChar->pushPacket(new CEntityUpdatePacket(PPet, ENTITY_SPAWN));
			}
		}
		return;
	}
	ShowError(CL_RED"CZone::InsertPET : entity is not pet\n" CL_RESET);
}

/************************************************************************
*																		*
*  Добавляем в зону активную область									*	 
*																		*
************************************************************************/

void CZone::InsertRegion(CRegion* Region)
{
	if (Region != NULL)
	{
		m_regionList.push_back(Region);
	}
}



/************************************************************************
*                                                                       *
*  Ищем группу для монстра. Для монстров, объединенных в группу         *
*  работает система взаимопомощи (link)                                 * 
*                                                                       *
************************************************************************/

void CZone::FindPartyForMob(CBaseEntity* PEntity)
{
    DSP_DEBUG_BREAK_IF(PEntity == NULL);
    DSP_DEBUG_BREAK_IF(PEntity->objtype != TYPE_MOB);

    CMobEntity* PMob = (CMobEntity*)PEntity;

    if (PMob->m_Link && PMob->PParty == NULL)
    {
        for (EntityList_t::const_iterator it = m_mobList.begin() ; it != m_mobList.end() ; ++it)
        {
            CMobEntity* PCurrentMob = (CMobEntity*)it->second;
            
            if (PCurrentMob->m_Link && 
                PCurrentMob->PMaster == NULL &&
                PCurrentMob->m_Family == PMob->m_Family)
            {
                PCurrentMob->PParty->AddMember(PMob);
                return;
            }
        }
        PMob->PParty = new CParty(PMob);
    }
}

/************************************************************************
*                                                                       *
*  Транспотр отправляется, необходимо собрать пассажиров                *
*                                                                       *
************************************************************************/

void CZone::TransportDepart(CBaseEntity* PTransportNPC)
{
    for (EntityList_t::const_iterator it = m_charList.begin() ; it != m_charList.end() ; ++it)
	{
		CCharEntity* PCurrentChar = (CCharEntity*)it->second;

        if (PCurrentChar->loc.boundary == PTransportNPC->loc.boundary)
        {
            luautils::OnTransportEvent(PCurrentChar, PTransportNPC->loc.prevzone);
        }
    }
}

/************************************************************************
*																		*
*  Удаляем персонажа из зоны. Если запущен ZoneServer и персонажей		*
*  в зоне больше не осталось, то останавливаем ZoneServer				*
*																		*
************************************************************************/

void CZone::DecreaseZoneCounter(CCharEntity* PChar)
{
    DSP_DEBUG_BREAK_IF(PChar == NULL);
    DSP_DEBUG_BREAK_IF(PChar->loc.zone != this);
	//remove pets
	if(PChar->PPet!=NULL){
		charutils::BuildingCharPetAbilityTable(PChar,(CPetEntity*)PChar->PPet,0);//blank the pet commands
		PChar->PPet->status = STATUS_DISAPPEAR;
		PChar->PPet->PBattleAI->SetCurrentAction(ACTION_NONE);
		DeletePET(PChar->PPet);//remove the TID for this pet
		for (EntityList_t::const_iterator it = m_charList.begin() ; it != m_charList.end() ; ++it)
		{
			//inform other players of the pets removal
			CCharEntity* PCurrentChar = (CCharEntity*)it->second;
			SpawnIDList_t::iterator PET = PCurrentChar->SpawnPETList.find(PChar->PPet->id);

			if( PET != PCurrentChar->SpawnPETList.end() )
			{
				PCurrentChar->SpawnPETList.erase(PET);
				PCurrentChar->pushPacket(new CEntityUpdatePacket(PChar->PPet, ENTITY_DESPAWN));
			}
		}
		PChar->PPet = NULL;
	}

	//remove bcnm status
	if(m_InstanceHandler!=NULL && PChar->StatusEffectContainer->HasStatusEffect(EFFECT_BATTLEFIELD)){
		if(m_InstanceHandler->disconnectFromBcnm(PChar)){
			ShowDebug("Removed %s from the BCNM they were in as they have left the zone.\n",PChar->GetName());
		}

		if(PChar->loc.destination==0){ //this player is disconnecting/logged out, so move them to the entrance
			//move depending on zone
			int* pos = instanceutils::getStartPosition(m_zoneID);
			if(pos!=NULL){
				PChar->loc.p.x = pos[0];
				PChar->loc.p.y = pos[1];
				PChar->loc.p.z = pos[2];
				PChar->loc.p.rotation = pos[3];
				charutils::SaveCharPosition(PChar);
			}
			else{
				ShowWarning("%s has disconnected from the BCNM but cannot move them to the lobby as the lobby position is unknown!\n",PChar->GetName());
			}
		}
	}

	for (EntityList_t::const_iterator it = m_mobList.begin() ; it != m_mobList.end() ; ++it)
	{
		CMobEntity* PCurrentMob = (CMobEntity*)it->second;
		PCurrentMob->PEnmityContainer->Clear(PChar->id);
	}

    // TODO: могут возникать проблемы с переходом между одной и той же зоной (zone == prevzone)

	m_charList.erase(PChar->targid);
	ShowDebug(CL_CYAN"CZone:: %s DecreaseZoneCounter <%u> %s\n" CL_RESET, GetName(), m_charList.size(),PChar->GetName());

	if (ZoneTimer && m_charList.empty())
	{
		ZoneTimer->m_type = CTaskMgr::TASK_REMOVE;
		ZoneTimer = NULL;
	} 
	else 
	{
		for (EntityList_t::const_iterator it = m_charList.begin() ; it != m_charList.end() ; ++it)
		{
			CCharEntity* PCurrentChar = (CCharEntity*)it->second;
			SpawnIDList_t::iterator PC = PCurrentChar->SpawnPCList.find(PChar->id);

			if( PC != PCurrentChar->SpawnPCList.end() )
			{
				PCurrentChar->SpawnPCList.erase(PC);
				PCurrentChar->pushPacket(new CCharPacket(PChar,ENTITY_DESPAWN));
			}
		}
	}
	if (PChar->m_LevelRestriction != 0)
	{
		PChar->StatusEffectContainer->DelStatusEffect(EFFECT_LEVEL_SYNC);
		PChar->StatusEffectContainer->DelStatusEffect(EFFECT_LEVEL_RESTRICTION);
	}
	if (PChar->PTreasurePool != NULL) // TODO: условие для устранения проблем с MobHouse, надо блин решить ее раз и навсегда
	{
		PChar->PTreasurePool->DelMember(PChar);
	}
    for (regionList_t::const_iterator region = m_regionList.begin(); region != m_regionList.end(); ++region)
    {
        if ((*region)->GetRegionID() == PChar->m_InsideRegionID)
        {
            luautils::OnRegionLeave(PChar, *region);
            break;
        }
    }
	
    PChar->loc.zone = NULL;
    PChar->loc.prevzone = m_zoneID;

    PChar->SpawnPCList.clear();
	PChar->SpawnNPCList.clear();
	PChar->SpawnMOBList.clear();
	PChar->SpawnPETList.clear();
}

/************************************************************************
*																		*
*  Добавляем персонажа в зону. Если ZoneServer не запущен то запускам.	*
*  Обязательно проверяем количество персонажей в зоне.					*
*  Максимальное число персонажей в одной зоне - 768                     *
*																		*
************************************************************************/

void CZone::IncreaseZoneCounter(CCharEntity* PChar)
{
	DSP_DEBUG_BREAK_IF(PChar == NULL);
    DSP_DEBUG_BREAK_IF(PChar->loc.zone != NULL);
	DSP_DEBUG_BREAK_IF(PChar->PTreasurePool != NULL);

    // ищем свободный targid для входящего в зону персонажа

    PChar->targid = 0x400;

    for (EntityList_t::const_iterator it = m_charList.begin() ; it != m_charList.end() ; ++it)
	{
        if (PChar->targid != it->first)
        {
            break;
        }
        PChar->targid++;
    }
    if (PChar->targid >= 0x700)
    {
        ShowError(CL_RED"CZone::InsertChar : targid is high (03hX)\n" CL_RESET, PChar->targid);
        return;
    }
    PChar->loc.zone = this;
    PChar->loc.zoning = false;
    PChar->loc.destination = 0;
    PChar->m_InsideRegionID = 0;
    PChar->m_PVPFlag = 0;

	m_charList[PChar->targid] = PChar;
	ShowDebug(CL_CYAN"CZone:: %s IncreaseZoneCounter <%u> %s \n" CL_RESET, GetName(), m_charList.size(),PChar->GetName());

	if (!ZoneTimer && !m_charList.empty())
	{
		ZoneTimer = CTaskMgr::getInstance()->AddTask(
			m_zoneName,
			gettick(),
			this,
			CTaskMgr::TASK_INTERVAL,
			m_regionList.empty() ? zone_server : zone_server_region,
			500);
	}
	
	//remove status effects that wear on zone
	PChar->StatusEffectContainer->DelStatusEffectsByFlag(EFFECTFLAG_ON_ZONE);

    if (PChar->animation == ANIMATION_CHOCOBO && !CanUseMisc(MISC_CHOCOBO))
    {
        PChar->animation = ANIMATION_NONE;
        PChar->StatusEffectContainer->DelStatusEffect(EFFECT_CHOCOBO);
    }
    if (PChar->m_Costum != 0)
    {
        PChar->m_Costum = 0;
        PChar->StatusEffectContainer->DelStatusEffect(EFFECT_COSTUME);
    }
	if (m_TreasurePool != NULL)
	{
		PChar->PTreasurePool = m_TreasurePool;
		PChar->PTreasurePool->AddMember(PChar);
	}
	else if (PChar->PParty != NULL)
	{
		PChar->PParty->ReloadTreasurePool(PChar);
	}
	else 
	{
		PChar->PTreasurePool = new CTreasurePool(TREASUREPOOL_SOLO);
		PChar->PTreasurePool->AddMember(PChar);
	}
}

/************************************************************************
Adds enmity to PSource for all the MOB targets who have PTarget on their
enmity list. 
************************************************************************/

void CZone::GenerateCureEnmity(CBattleEntity* PSource,CBattleEntity* PTarget,uint16 amount){
	DSP_DEBUG_BREAK_IF(PSource == NULL);
	DSP_DEBUG_BREAK_IF(PTarget == NULL);
	DSP_DEBUG_BREAK_IF(amount < 0);
	DSP_DEBUG_BREAK_IF(PSource->objtype != TYPE_PC);
	
	CCharEntity* PChar = (CCharEntity*)PSource;
	
	for (EntityList_t::const_iterator it = m_mobList.begin() ; it != m_mobList.end() ; ++it)
	{
		CMobEntity* PCurrentMob = (CMobEntity*)it->second;
		float CurrentDistance = distance(PChar->loc.p, PCurrentMob->loc.p);
		//cures only generate hate for monsters within 40" of the target of the cure who have
		//the target on their list
		if(CurrentDistance<40){
			if(PCurrentMob->PEnmityContainer->HasTargetID(PTarget->id)){
				//add for PSource
				if(amount==65535){ //cure v
					PCurrentMob->PEnmityContainer->UpdateEnmityFromCure(PChar,PTarget->GetMLevel(),amount,true);
				}
				else{
					PCurrentMob->PEnmityContainer->UpdateEnmityFromCure(PChar,PTarget->GetMLevel(),amount,false);
				}
			}
		}
	}
}

/************************************************************************
*																		*
*  Проверка видимости монстров персонажем. Дистанцию лучше вынести в	*
*  глобальную переменную (настройки сервера)							*
*  Именно в этой функции будем проверять агрессию мостров, чтобы не		*
*  вычислять distance несколько раз (например в ZoneServer)				*
*																		*
************************************************************************/

void CZone::SpawnMOBs(CCharEntity* PChar)
{
	for (EntityList_t::const_iterator it = m_mobList.begin() ; it != m_mobList.end() ; ++it)
	{
		CMobEntity* PCurrentMob = (CMobEntity*)it->second;
        SpawnIDList_t::iterator MOB = PChar->SpawnMOBList.lower_bound(PCurrentMob->id);

		float CurrentDistance = distance(PChar->loc.p, PCurrentMob->loc.p);

		if (PCurrentMob->status == STATUS_UPDATE &&
			CurrentDistance < 50) 
		{
			if( MOB == PChar->SpawnMOBList.end() ||
				PChar->SpawnMOBList.key_comp()(PCurrentMob->id, MOB->first))
			{
				PChar->SpawnMOBList.insert(MOB, SpawnIDList_t::value_type(PCurrentMob->id, PCurrentMob));
				PChar->pushPacket(new CEntityUpdatePacket(PCurrentMob, ENTITY_SPAWN));
			}

			if (PChar->isDead() ||
                PChar->nameflags.flags & FLAG_GM) 
				continue; 

            // проверка ночного/дневного сна монстров уже учтена в проверке CurrentAction, т.к. во сне монстры не ходят ^^

            if (PCurrentMob->m_Behaviour != BEHAVIOUR_NONE &&
                PCurrentMob->PMaster == NULL &&
				PCurrentMob->PBattleAI->GetCurrentAction() == ACTION_ROAMING &&
				CurrentDistance < 20)
			{
				if (PChar->animation != ANIMATION_CHOCOBO &&
				   (PChar->animation == ANIMATION_HEALING ||
				   (int8)(PChar->GetMLevel() - PCurrentMob->GetMLevel()) < 10))
				{
                    //CurrentDistance += PChar->getMod(MOD_STEALTH);

					if (PCurrentMob->m_Behaviour & BEHAVIOUR_AGGRO_SIGHT && 
                      !(PChar->StatusEffectContainer->HasStatusEffect(EFFECT_INVISIBLE) || 
                        PChar->StatusEffectContainer->HasStatusEffect(EFFECT_HIDE) || 
                        PChar->StatusEffectContainer->HasStatusEffect(EFFECT_CAMOUFLAGE)))
					{
                        if (CurrentDistance < 15 &&
							isFaceing(PCurrentMob->loc.p, PChar->loc.p, 40))
						{
							PCurrentMob->PEnmityContainer->AddBaseEnmity(PChar);
							continue;
						}
					}
					if (PCurrentMob->m_Behaviour & BEHAVIOUR_AGGRO_HEARING && 
                       !PChar->StatusEffectContainer->HasStatusEffect(EFFECT_SNEAK))
					{
						if (CurrentDistance < 8)
						{
							PCurrentMob->PEnmityContainer->AddBaseEnmity(PChar);
							continue;
						} 
					}
					if (PCurrentMob->m_Behaviour & BEHAVIOUR_AGGRO_LOWHP && 
                       !PChar->StatusEffectContainer->HasStatusEffect(EFFECT_DEODORIZE))
					{
						if (PChar->GetHPP() < 66)
						{
							PCurrentMob->PEnmityContainer->AddBaseEnmity(PChar);
							continue;
						}
					}
					if (PCurrentMob->m_Behaviour & BEHAVIOUR_AGGRO_TRUESIGHT)
					{
						if (CurrentDistance < 15)
						{
							PCurrentMob->PEnmityContainer->AddBaseEnmity(PChar);
							continue;
						}
					}
					if (PCurrentMob->m_Behaviour & BEHAVIOUR_AGGRO_TRUEHEARING)
					{
						if (CurrentDistance < 8)
						{
							PCurrentMob->PEnmityContainer->AddBaseEnmity(PChar);
							continue;
						}
					}
					if (PCurrentMob->m_Behaviour & BEHAVIOUR_AGGRO_MAGIC)
					{
						if (PChar->PBattleAI->GetCurrentAction() == ACTION_MAGIC_CASTING)
						{
							if(PChar->PBattleAI->GetCurrentSpell()->getSpellGroup()!=SPELLGROUP_SONG &&
								PChar->PBattleAI->GetCurrentSpell()->getSpellGroup()!=SPELLGROUP_NINJUTSU){
								PCurrentMob->PEnmityContainer->AddBaseEnmity(PChar);
								continue;
							}
						}
					}
					if (PCurrentMob->m_Behaviour & BEHAVIOUR_AGGRO_WEAPONSKILL)
					{
						if (PChar->PBattleAI->GetCurrentAction() == ACTION_WEAPONSKILL_START)
						{
							PCurrentMob->PEnmityContainer->AddBaseEnmity(PChar);
							continue;
						}
					}
					if (PCurrentMob->m_Behaviour & BEHAVIOUR_AGGRO_JOBABILITY)
					{
						if (PChar->PBattleAI->GetCurrentAction() == ACTION_JOBABILITY_START)
						{
                            PCurrentMob->PEnmityContainer->AddBaseEnmity(PChar);
							continue;
						}
					}
				}
			}
		}
        else
        {
			if( MOB != PChar->SpawnMOBList.end() &&
			  !(PChar->SpawnMOBList.key_comp()(PCurrentMob->id, MOB->first)))
			{
				PChar->SpawnMOBList.erase(MOB);
				PChar->pushPacket(new CEntityUpdatePacket(PCurrentMob,ENTITY_DESPAWN));
			}
		}
	}
}

/************************************************************************
*																		*
*  Проверка видимости питомцев персонажем. Для появления питомцев		*
*  используем UPDATE вместо SPAWN. SPAWN используется лишь при вызове	*
*																		*
************************************************************************/

void CZone::SpawnPETs(CCharEntity* PChar)
{
	for (EntityList_t::const_iterator it = m_petList.begin() ; it != m_petList.end() ; ++it)
	{
		CPetEntity* PCurrentPet = (CPetEntity*)it->second;
		SpawnIDList_t::iterator PET = PChar->SpawnPETList.lower_bound(PCurrentPet->id);

		if ((PCurrentPet->status == STATUS_NORMAL || PCurrentPet->status == STATUS_UPDATE) &&
			distance(PChar->loc.p, PCurrentPet->loc.p) < 50) 
		{
			if( PET == PChar->SpawnPETList.end() ||
				PChar->SpawnPETList.key_comp()(PCurrentPet->id, PET->first))
			{
				PChar->SpawnPETList.insert(PET, SpawnIDList_t::value_type(PCurrentPet->id, PCurrentPet));
				PChar->pushPacket(new CEntityUpdatePacket(PCurrentPet,ENTITY_UPDATE));
			}
		}else{
			if( PET != PChar->SpawnPETList.end() &&
			  !(PChar->SpawnPETList.key_comp()(PCurrentPet->id, PET->first)))
			{
				PChar->SpawnPETList.erase(PET);
				PChar->pushPacket(new CEntityUpdatePacket(PCurrentPet,ENTITY_DESPAWN));
			}
		}
	}
}

/************************************************************************
*																		*
*  Проверка видимости NPCs персонажем.									*
*																		*
************************************************************************/

void CZone::SpawnNPCs(CCharEntity* PChar)
{
	for (EntityList_t::const_iterator it = m_npcList.begin() ; it != m_npcList.end() ; ++it)
	{
		CNpcEntity* PCurrentNpc = (CNpcEntity*)it->second;
		SpawnIDList_t::iterator NPC = PChar->SpawnNPCList.lower_bound(PCurrentNpc->id);
		
		if (PCurrentNpc->status == STATUS_NORMAL)
		{
			if(distance(PChar->loc.p, PCurrentNpc->loc.p) < 50) 
			{
				if( NPC == PChar->SpawnNPCList.end() ||
					PChar->SpawnNPCList.key_comp()(PCurrentNpc->id, NPC->first))
				{
					PChar->SpawnNPCList.insert(NPC, SpawnIDList_t::value_type(PCurrentNpc->id, PCurrentNpc));
					PChar->pushPacket(new CEntityUpdatePacket(PCurrentNpc,ENTITY_SPAWN));
				}
			}else{
				if( NPC != PChar->SpawnNPCList.end() &&
				  !(PChar->SpawnNPCList.key_comp()(PCurrentNpc->id, NPC->first)))
				{
					PChar->SpawnNPCList.erase(NPC);
					PChar->pushPacket(new CEntityUpdatePacket(PCurrentNpc,ENTITY_DESPAWN));
				}
			}
		}
	}
}

/************************************************************************
*																		*
*  Проверка видимости персонажей. Смысл действий в том, что персонажи	*
*  сами себя обновляют и добавляются в списки других персонажей.        *
*  В оригинальной версии размер списка ограничен и изменяется в			*
*  пределах 25-50 видимых персонажей.									*
*																		*
************************************************************************/

void CZone::SpawnPCs(CCharEntity* PChar)
{
	for (EntityList_t::const_iterator it = m_charList.begin() ; it != m_charList.end() ; ++it)
	{
		CCharEntity* PCurrentChar = (CCharEntity*)it->second;
		SpawnIDList_t::iterator PC = PChar->SpawnPCList.find(PCurrentChar->id);

		if (PChar != PCurrentChar)
		{
			if(distance(PChar->loc.p, PCurrentChar->loc.p) < 50) 
			{
				if( PC == PChar->SpawnPCList.end() )
				{
					PChar->SpawnPCList[PCurrentChar->id] = PCurrentChar;
					PChar->pushPacket(new CCharPacket(PCurrentChar,ENTITY_SPAWN));

					PCurrentChar->SpawnPCList[PChar->id] = PChar;
					PCurrentChar->pushPacket(new CCharPacket(PChar,ENTITY_SPAWN));
				}else{
					PCurrentChar->pushPacket(new CCharPacket(PChar,ENTITY_UPDATE));
				}
			} else {
				if( PC != PChar->SpawnPCList.end() )
				{
					PChar->SpawnPCList.erase(PC);
					PChar->pushPacket(new CCharPacket(PCurrentChar,ENTITY_DESPAWN));

					PCurrentChar->SpawnPCList.erase(PChar->id);
					PCurrentChar->pushPacket(new CCharPacket(PChar,ENTITY_DESPAWN));
				}
			}
		}
	}
}

/************************************************************************
*																		*
*  Отображаем Moogle в MogHouse											*
*																		*
************************************************************************/

void CZone::SpawnMoogle(CCharEntity* PChar)
{
	for (EntityList_t::const_iterator it = m_npcList.begin() ; it != m_npcList.end() ; ++it)
	{
		CNpcEntity* PCurrentNpc = (CNpcEntity*)it->second;
		
		if( PCurrentNpc->loc.p.z == 1.5 &&
			PCurrentNpc->look.face == 0x52)
		{
			PCurrentNpc->status = STATUS_NORMAL;
			PChar->pushPacket(new CEntityUpdatePacket(PCurrentNpc,ENTITY_SPAWN));
			PCurrentNpc->status = STATUS_DISAPPEAR;
			return;
		}
	}
}

/************************************************************************
*                                                                       *
*  Отображаем транспотр в зоне (не хранится в основном списке)          *
*                                                                       *
************************************************************************/

void CZone::SpawnTransport(CCharEntity* PChar)
{
	if (m_Transport != NULL)
    {
		PChar->pushPacket(new CEntityUpdatePacket(m_Transport, ENTITY_SPAWN));
	    return;
    }
}

/************************************************************************
*																		*
*  Получаем указатель на любую сущность в зоне по ее targid				*
*																		*
************************************************************************/

CBaseEntity* CZone::GetEntity(uint16 targid, uint8 filter)
{
	CBaseEntity* PEntity = NULL;

	if (targid < 0x400)
	{
		if (filter & TYPE_MOB)
		{
			EntityList_t::const_iterator it = m_mobList.find(targid); 
			if (it != m_mobList.end())
			{
				PEntity = it->second;
			}
		}
		if (filter & TYPE_NPC)
		{
			EntityList_t::const_iterator it = m_npcList.find(targid); 
			if (it != m_npcList.end())
			{
				PEntity = it->second;
			}
		}
        if (filter & TYPE_SHIP)
        {
            if (m_Transport != NULL && m_Transport->targid == targid)
            {
                PEntity = m_Transport;
            }
        }
	}
	else if (targid < 0x700)
	{
		if (filter & TYPE_PC)
		{
			EntityList_t::const_iterator it = m_charList.find(targid); 
			if (it != m_charList.end())
			{
				PEntity = it->second;
			}
		}
	}
	else if (targid < 0x800)
	{
		if (filter & TYPE_PET)
		{
			EntityList_t::const_iterator it = m_petList.find(targid); 
			if (it != m_petList.end())
			{
				PEntity = it->second;
			}	
		}
	}
	return PEntity;
}

/************************************************************************
*																		*
*  Oбработка реакции мира на смену времени суток						*
*																		*
************************************************************************/

void CZone::TOTDChange(TIMETYPE TOTD)
{
	SCRIPTTYPE ScriptType = SCRIPT_NONE;

	switch (TOTD)
	{
        case TIME_FOG:
        {
            for (EntityList_t::const_iterator it = m_mobList.begin(); it != m_mobList.end(); ++it)
			{
				CMobEntity* PMob = (CMobEntity*)it->second;

				if (PMob->m_SpawnType == SPAWNTYPE_FOG)
				{
                    PMob->PBattleAI->SetCurrentAction(ACTION_SPAWN);
				}
			}
        }
        break;
		case TIME_NEWDAY:
		{
			for (EntityList_t::const_iterator it = m_mobList.begin(); it != m_mobList.end(); ++it)
			{
				CMobEntity* PMob = (CMobEntity*)it->second;

                if (PMob->m_SpawnType == SPAWNTYPE_ATNIGHT)
                {
                    PMob->PBattleAI->SetLastActionTime(gettick() - 12000);
					PMob->PBattleAI->SetCurrentAction(ACTION_DEATH);
                }
			}
		}
		break;
		case TIME_DAWN:
		{
			ScriptType = SCRIPT_TIME_DAWN;
			
			for (EntityList_t::const_iterator it = m_mobList.begin(); it != m_mobList.end(); ++it)
			{
				CMobEntity* PMob = (CMobEntity*)it->second;

				if (PMob->m_SpawnType == SPAWNTYPE_ATEVENING)
				{
                    PMob->PBattleAI->SetLastActionTime(gettick() - 12000);
					PMob->PBattleAI->SetCurrentAction(ACTION_DEATH);
				}
			}
		}
		break;
		case TIME_DAY:
		{
			ScriptType = SCRIPT_TIME_DAY;

            for (EntityList_t::const_iterator it = m_mobList.begin(); it != m_mobList.end(); ++it)
			{
				CMobEntity* PMob = (CMobEntity*)it->second;

                if (PMob->m_SpawnType ==  SPAWNTYPE_FOG)
                {
                    PMob->PBattleAI->SetLastActionTime(gettick() - 12000);
					PMob->PBattleAI->SetCurrentAction(ACTION_DEATH);
                }
			}
		}
		break;
		case TIME_DUSK:
		{
			ScriptType = SCRIPT_TIME_DUSK;
		}
		break;
		case TIME_EVENING:
		{
			ScriptType = SCRIPT_TIME_EVENING;
			
			for (EntityList_t::const_iterator it = m_mobList.begin(); it != m_mobList.end(); ++it)
			{
				CMobEntity* PMob = (CMobEntity*)it->second;

				if (PMob->m_SpawnType == SPAWNTYPE_ATEVENING)
				{
					PMob->PBattleAI->SetCurrentAction(ACTION_SPAWN);
				}
			}
		}
		break;
		case TIME_NIGHT:
		{
			for (EntityList_t::const_iterator it = m_mobList.begin(); it != m_mobList.end(); ++it)
			{
				CMobEntity* PMob = (CMobEntity*)it->second;

				if (PMob->m_SpawnType == SPAWNTYPE_ATNIGHT)
				{
					PMob->PBattleAI->SetCurrentAction(ACTION_SPAWN);
				}
			}
		}
		break;
    }
	if (ScriptType != SCRIPT_NONE)
	{
		for (EntityList_t::const_iterator it = m_charList.begin(); it != m_charList.end(); ++it)
		{
			charutils::CheckEquipLogic((CCharEntity*)it->second, ScriptType, TOTD);
		}
	}
}

CCharEntity* CZone::FindPlayerInZone(char* name){
	if(m_charList.empty()){
		return NULL;
	}

	for (EntityList_t::const_iterator it = m_charList.begin() ; it != m_charList.end() ; ++it)
	{
		CCharEntity* PCurrentChar = (CCharEntity*)it->second;
		if(strcmp(PCurrentChar->GetName(),name)==0){
			return PCurrentChar;
		}
	}
	return NULL;
}

/************************************************************************
*																		*
*  Отправляем глобальные пакеты											*
*																		*
************************************************************************/

void CZone::PushPacket(CBaseEntity* PEntity, GLOBAL_MESSAGE_TYPE message_type, CBasicPacket* packet)
{
	if (!m_charList.empty())
	{
		switch(message_type)
		{
			case CHAR_INRANGE_SELF :
			{
				if (PEntity->objtype == TYPE_PC)
				{
					((CCharEntity*)PEntity)->pushPacket(new CBasicPacket(*packet));
				}
			}
			case CHAR_INRANGE :
			{
				for (EntityList_t::const_iterator it = m_charList.begin() ; it != m_charList.end() ; ++it)
				{
					CCharEntity* PCurrentChar = (CCharEntity*)it->second;
					if (PEntity != PCurrentChar)
					{
						if(distance(PEntity->loc.p, PCurrentChar->loc.p) < 50) 
						{
							PCurrentChar->pushPacket(new CBasicPacket(*packet));
						}
					}
				}
			}
				break;
			case CHAR_INSHOUT :
			{
				for (EntityList_t::const_iterator it = m_charList.begin() ; it != m_charList.end() ; ++it)
				{
					CCharEntity* PCurrentChar = (CCharEntity*)it->second;
					if (PEntity != PCurrentChar)
					{
						if(distance(PEntity->loc.p, PCurrentChar->loc.p) < 180) 
						{
							PCurrentChar->pushPacket(new CBasicPacket(*packet));
						}
					}
				}
			}
				break;
			case CHAR_INZONE :
			{
				for (EntityList_t::const_iterator it = m_charList.begin() ; it != m_charList.end() ; ++it)
				{
					CCharEntity* PCurrentChar = (CCharEntity*)it->second;
					if (PEntity != PCurrentChar)
					{
						PCurrentChar->pushPacket(new CBasicPacket(*packet));
					}
				}
			}
				break;
		}
	}
	delete packet;
}

/************************************************************************
*																		*
*  Wide Scan															*
*																		*
************************************************************************/

void CZone::WideScan(CCharEntity* PChar, uint16 radius)
{
	PChar->pushPacket(new CWideScanPacket(WIDESCAN_BEGIN));
	for (EntityList_t::const_iterator it = m_npcList.begin() ; it != m_npcList.end() ; ++it)
	{
		if(distance(PChar->loc.p, (it->second)->loc.p) < radius) 
		{
			PChar->pushPacket(new CWideScanPacket(PChar,it->second)); // потом добавлю условие на статус и видимость имени
		}
	}
	for (EntityList_t::const_iterator it = m_mobList.begin() ; it != m_mobList.end() ; ++it)
	{
		if(distance(PChar->loc.p, (it->second)->loc.p) < radius) 
		{
			PChar->pushPacket(new CWideScanPacket(PChar,it->second));
		}
	}
	PChar->pushPacket(new CWideScanPacket(WIDESCAN_END));
}

/************************************************************************
*																		*
*  Cервер для обработки активности и статус-эффектов сущностей в зоне.	*
*  При любом раскладе последними должны обрабатываться персонажи		*
*																		*
************************************************************************/

void CZone::ZoneServer(uint32 tick)
{
	for (EntityList_t::const_iterator it = m_mobList.begin() ; it != m_mobList.end() ; ++it)
	{
		CMobEntity* PMob = (CMobEntity*)it->second;

		PMob->StatusEffectContainer->CheckEffects(tick);
		PMob->PBattleAI->CheckCurrentAction(tick);
	}


	EntityList_t::const_iterator pit = m_petList.begin();
	while(pit != m_petList.end())
	{
		CPetEntity* PPet = (CPetEntity*)pit->second;
		PPet->StatusEffectContainer->CheckEffects(tick);
		PPet->PBattleAI->CheckCurrentAction(tick);
		if(PPet->status==STATUS_DISAPPEAR){
			m_petList.erase(pit++);
		}
		else{
			++pit;
		}
	}

	if(m_InstanceHandler!=NULL){
		m_InstanceHandler->handleInstances(tick);
	}

    for (EntityList_t::const_iterator it = m_charList.begin() ; it != m_charList.end() ; ++it)
    {
        CCharEntity* PChar = (CCharEntity*)it->second;

        PChar->StatusEffectContainer->CheckEffects(tick);
        PChar->PBattleAI->CheckCurrentAction(tick);
        PChar->PTreasurePool->CheckItems(tick);
    }
}

/************************************************************************
*																		*
*  Cервер для обработки активности и статус-эффектов сущностей в зоне.	*
*  Дополнительно обрабатывается проверка на вход и выход персонажей из	*
*  активных областей (пока реализован только вход в область).			*
*  При любом раскладе последними должны обрабатываться персонажи		*
*																		*
************************************************************************/

void CZone::ZoneServerRegion(uint32 tick)
{
	for (EntityList_t::const_iterator it = m_mobList.begin() ; it != m_mobList.end() ; ++it)
	{
		CMobEntity* PMob = (CMobEntity*)it->second;

		PMob->StatusEffectContainer->CheckEffects(tick);
		PMob->PBattleAI->CheckCurrentAction(tick);
	}

	for (EntityList_t::const_iterator it = m_petList.begin() ; it != m_petList.end() ; ++it)
	{
		CPetEntity* PPet = (CPetEntity*)it->second;

		PPet->StatusEffectContainer->CheckEffects(tick);
		PPet->PBattleAI->CheckCurrentAction(tick);
	}

    for (EntityList_t::const_iterator it = m_charList.begin() ; it != m_charList.end() ; ++it)
    {
        CCharEntity* PChar = (CCharEntity*)it->second;
        if (PChar->status != STATUS_SHUTDOWN)
        {
            PChar->StatusEffectContainer->CheckEffects(tick);
            PChar->PBattleAI->CheckCurrentAction(tick);
            PChar->PTreasurePool->CheckItems(tick);

            uint32 RegionID = 0;

            for (regionList_t::const_iterator region = m_regionList.begin(); region != m_regionList.end(); ++region)
            {
                if ((*region)->isPointInside(PChar->loc.p))
                {
                    RegionID = (*region)->GetRegionID();

                    if ((*region)->GetRegionID() != PChar->m_InsideRegionID)
                    {
                        luautils::OnRegionEnter(PChar, *region);
                    }
                    if (PChar->m_InsideRegionID == 0) break;
                }
                else if ((*region)->GetRegionID() == PChar->m_InsideRegionID)
                {
                    luautils::OnRegionLeave(PChar, *region);
                }
            }
            PChar->m_InsideRegionID = RegionID;
        }
	}
}

//===========================================================

/*
id				CBaseEntity
name			CBaseEntity		
pos_rot			CBaseEntity
pos_x			CBaseEntity
pos_y			CBaseEntity
pos_z			CBaseEntity
speed			CBaseEntity
speedsub		CBaseEntity
animation		CBaseEntity
animationsub	CBaseEntity
namevis			npc+mob
status			CBaseEntity
unknown
look			CBaseEntity
name_prefix
*/
