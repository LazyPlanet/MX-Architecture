#include <vector>
#include <algorithm>

#include <cpp_redis/cpp_redis>

#include "Game.h"
#include "Timer.h"
#include "Asset.h"
#include "MXLog.h"
#include "CommonUtil.h"
#include "RedisManager.h"

namespace Adoter
{

extern const Asset::CommonConst* g_const;

/////////////////////////////////////////////////////
//一场游戏
/////////////////////////////////////////////////////
void Game::Init(std::shared_ptr<Room> room)
{
	_cards.clear();
	_cards.resize(136);

	std::iota(_cards.begin(), _cards.end(), 1);
	std::vector<int32_t> cards(_cards.begin(), _cards.end());

	std::random_shuffle(cards.begin(), cards.end()); //洗牌

	_cards = std::list<int32_t>(cards.begin(), cards.end());

	_room = room;
}

bool Game::Start(std::vector<std::shared_ptr<Player>> players, int64_t room_id, int32_t game_id)
{
	if (MAX_PLAYER_COUNT != players.size()) return false; //做下检查，是否满足开局条件

	if (!_room) return false;

	_game_id = game_id + 1;
	_room_id = room_id;

	//
	//房间(Room)其实是游戏(Game)的管理类
	//
	//复制房间数据到游戏中
	//
	int32_t player_index = 0;

	for (auto player : players)
	{
		_players[player_index++] = player; 

		player->SetRoom(_room);
		player->SetPosition(Asset::POSITION_TYPE(player_index));
		player->OnGameStart();
	}

	//当前庄家
	auto banker_index = _room->GetBankerIndex();
		
	auto player_banker = GetPlayerByOrder(banker_index);
	if (!player_banker) return false;

	_banker_player_id  = player_banker->GetID();
	
	OnStart(); //同步本次游戏开局数据：此时玩家没有进入游戏

	//
	//设置游戏数据
	//
	for (int i = 0; i < MAX_PLAYER_COUNT; ++i)
	{
		auto player = _players[i];
		if (!player) return false;

		player->SetGame(shared_from_this());

		int32_t card_count = 13; //正常开启，普通玩家牌数量

		if (banker_index % MAX_PLAYER_COUNT == i) 
		{
			card_count = 14; //庄家牌数量
			_curr_player_index = i; //当前操作玩家
		}

		//玩家发牌
		auto cards = FaPai(card_count);
		player->OnFaPai(cards);  

		//回放缓存：初始牌数据
		_playback.mutable_options()->CopyFrom(_room->GetOptions()); //房间玩法
		auto player_element = _playback.mutable_player_list()->Add();
		player_element->set_player_id(player->GetID());
		player_element->set_position(player->GetPosition());
		player_element->mutable_common_prop()->CopyFrom(player->CommonProp());
		player_element->mutable_wechat()->CopyFrom(player->GetWechat());

		const auto& cards_inhand = player->GetCardsInhand();
		for (const auto& crds : cards_inhand)
		{
			auto pai_list = player_element->mutable_pai_list()->Add();
			pai_list->set_card_type((Asset::CARD_TYPE)crds.first); //牌类型
			for (auto card_value : crds.second) pai_list->mutable_cards()->Add(card_value); //牌值
		}
	}

	return true;
}
	
void Game::OnStart()
{
	if (!_room) return;

	_room->SetBanker(_banker_player_id); //设置庄家

	Asset::GameInformation info; //游戏数据广播
	info.set_banker_player_id(_banker_player_id);
	BroadCast(info);
	
	Asset::RandomSaizi saizi; //开局股子广播
	saizi.set_reason_type(Asset::RandomSaizi_REASON_TYPE_REASON_TYPE_START);

	for (int i = 0; i < 2; ++i)
	{
		int32_t result = CommonUtil::Random(1, 6);
		saizi.mutable_random_result()->Add(result);
		
		_saizi_random_result.push_back(result);
	}
	_room->BroadCast(saizi);
}

bool Game::OnGameOver(int64_t player_id)
{
	if (!_room) return false;

	SavePlayBack(); //回放

	for (auto player : _players)
	{
		if (!player) continue;

		player->OnGameOver();
	}
	
	ClearState();

	_room->OnGameOver(player_id); //胡牌

	return true;
}

void Game::SavePlayBack()
{
	if (!_room || _room->IsFriend()) return; //非好友房不存回放

	std::string key = "playback:" + std::to_string(_room_id) + "_" + std::to_string(_game_id);
	RedisInstance.Save(key, _playback);
}
	
void Game::ClearState()
{
	_baopai.Clear();

	_oper_cache.Clear();

	_oper_list.clear();

	_cards_pool.clear();
	
	_liuju = false;
}

/////////////////////////////////////////////////////
//
//玩家可操作的状态只有2种，顺序不可变：
//
//(1) 碰、杠、胡牌；
//
//(2) 轮到玩家；
//
/////////////////////////////////////////////////////
	
bool Game::CanPaiOperate(std::shared_ptr<Player> player)
{
	if (!player) return false;

	if (/*_oper_cache.time_out() < CommonTimerInstance.GetTime() && 超时检查*/_oper_cache.player_id() == player->GetID()) 
	{
		return true; //玩家操作：碰、杠、胡牌
	}

	auto player_index = GetPlayerOrder(player->GetID());
	if (player_index < 0) return false;

	if (_curr_player_index == player_index) 
	{
		return true; //轮到该玩家
	}
	
	LOG(ERROR, "房间:{} 局数:{} 当前缓存玩家索引:{} 当前操作玩家索引:{} 当前操作玩家:{} 服务器缓存数据:{}", 
			_room_id, _game_id, _curr_player_index, player_index, player->GetID(), _oper_cache.ShortDebugString());
	return false;
}
	
void Game::OnPlayerReEnter(std::shared_ptr<Player> player)
{
	if (!player) return;
	
	if (!CanPaiOperate(player)) return; //尚未轮到该玩家操作

	auto player_index = GetPlayerOrder(player->GetID());
	if (player_index < 0) return;

	if (_curr_player_index != player_index && _oper_cache.player_id() == player->GetID() && (_oper_cache.oper_list().size() > 0))
	{
		auto player_id = player->GetID();
		DEBUG("玩家:{}由于房间内断线重入房间，操作重新推送:{} 当前玩家索引:{} 操作玩家索引:{}", player_id, _oper_cache.ShortDebugString(), _curr_player_index, player_index);
		
		Asset::PaiOperationAlert alert;

		for (int32_t i = 0; i < _oper_cache.oper_list().size(); ++i)
		{
			if (_oper_cache.oper_list(i) == Asset::PAI_OPER_TYPE_TINGPAI)
			{
				for (const auto& pai : _oper_cache.ting_pais())
				{
					auto pai_perator = alert.mutable_pais()->Add();
					pai_perator->mutable_oper_list()->Add(Asset::PAI_OPER_TYPE_TINGPAI); //可操作牌类型
					pai_perator->mutable_pai()->CopyFrom(pai);
				}
			}
			else
			{
				auto pai_perator = alert.mutable_pais()->Add();
				pai_perator->mutable_oper_list()->Add(_oper_cache.oper_list(i)); //可操作牌类型
				pai_perator->mutable_pai()->CopyFrom(_oper_cache.pai());
			}
		}
						
		player->SendProtocol(alert);

		return;
	}

	//
	//如果缓存其他玩家操作，则认为没轮到自己操作
	//
	//比如，上家打牌，下家碰，打牌之后下家有吃碰操作的时候回来还会多一张牌出来
	//
	if (_oper_cache.player_id() != 0 && _oper_cache.player_id() != player->GetID()) return;

	auto cards = FaPai(1); 
	auto card = GameInstance.GetCard(cards[0]); //玩家待抓的牌
	
	if (player->ShouldZhuaPai()) player->OnFaPai(cards); //是否应该抓牌
		
	DEBUG("玩家:{}由于断线重入房间，进行发牌:{} 缓存操作:{} 当前索引:{} 玩家索引:{}", player->GetID(), card.ShortDebugString(), _oper_cache.ShortDebugString(), _curr_player_index, player_index);

	Asset::PaiOperationAlert alert;

	//
	//胡牌检查
	//
	//注意：自摸和其他玩家点炮之间的检查顺序
	//
	if (player->CheckZiMo(false)) //自摸检查
	{
		auto pai_perator = alert.mutable_pais()->Add();
		pai_perator->mutable_pai()->CopyFrom(player->GetZhuaPai());
		pai_perator->mutable_oper_list()->Add(Asset::PAI_OPER_TYPE_HUPAI);
					
		SetZiMoCache(player, card); //自摸胡牌缓存
	}
	//
	//玩家摸宝之后进行抓牌正好抓到宝胡
	//
	else if (player->HasPai(_baopai) && player->CheckBaoHu(card)) //宝胡
	{
		auto pai_perator = alert.mutable_pais()->Add();
		pai_perator->mutable_pai()->CopyFrom(_baopai);
		pai_perator->mutable_oper_list()->Add(Asset::PAI_OPER_TYPE_HUPAI);
		
		SetZiMoCache(player, card); //自摸胡牌缓存
	}

	//
	//听牌检查
	//
	std::vector<Asset::PaiElement> ting_list;
	if (player->CheckTingPai(ting_list))
	{
		_oper_cache.mutable_oper_list()->Add(Asset::PAI_OPER_TYPE_TINGPAI);

		for (auto pai : ting_list) 
		{
			auto pai_perator = alert.mutable_pais()->Add();
			pai_perator->mutable_pai()->CopyFrom(pai);
			pai_perator->mutable_oper_list()->Add(Asset::PAI_OPER_TYPE_TINGPAI);

			_oper_cache.mutable_ting_pais()->Add()->CopyFrom(pai);
		}
	}

	//
	//明杠和暗杠检查
	//
	::google::protobuf::RepeatedField<Asset::PaiOperationAlert_AlertElement> gang_list;
	if (player->CheckAllGangPai(gang_list)) 
	{
		for (auto gang : gang_list) 
		{
			auto pai_perator = alert.mutable_pais()->Add();
			pai_perator->CopyFrom(gang);
		
			_oper_cache.mutable_pai()->CopyFrom(gang.pai());
			for (auto oper_type : gang.oper_list()) _oper_cache.mutable_oper_list()->Add(Asset::PAI_OPER_TYPE(oper_type));
		}
	}
	
	//
	//旋风杠检查
	//
	
	if (player->GetFaPaiCount() == 1) player->CheckXuanFengGang(); //庄家多次断线旋风杠丢失
	
	auto xf_gang = player->CheckXuanFeng();
	if (xf_gang)
	{
		auto pai_perator = alert.mutable_pais()->Add();
		pai_perator->mutable_oper_list()->Add((Asset::PAI_OPER_TYPE)xf_gang);
			
		_oper_cache.mutable_oper_list()->Add((Asset::PAI_OPER_TYPE)xf_gang);
	}

	if (alert.pais().size()) 
	{
		player->SendProtocol(alert); //提示Client

		_oper_cache.set_player_id(player->GetID()); //当前可操作玩家
		_oper_cache.set_source_player_id(player->GetID()); //当前牌来自玩家，自己抓牌
		_oper_cache.set_time_out(CommonTimerInstance.GetTime() + 30); //8秒后超时
	}
}

void Game::OnPaiOperate(std::shared_ptr<Player> player, pb::Message* message)
{
	if (!player || !message || !_room) return;
	
	Asset::PaiOperation* pai_operate = dynamic_cast<Asset::PaiOperation*>(message);
	if (!pai_operate) return; 

	AddPlayerOperation(*pai_operate);  //回放记录
	
	auto player_index = GetPlayerOrder(player->GetID());

	auto curr_player_id = player->GetID();
	auto pai_operate_string = pai_operate->ShortDebugString();
	auto oper_limit_string = _oper_cache.ShortDebugString();

	DEBUG("房间:{} 当前牌局:{} 当前操作玩家ID:{} 位置索引:{} 进行的操作:{} 服务器记录的当前可操作玩家索引:{} 服务器缓存玩家操作:{}", _room_id, _game_id, curr_player_id, player_index, pai_operate_string, _curr_player_index, oper_limit_string);

	if (!CanPaiOperate(player)) 
	{
		player->AlertMessage(Asset::ERROR_GAME_NO_PERMISSION); //没有权限，没到玩家操作，防止外挂
		//return; //不允许操作
	}

	//if (CommonTimerInstance.GetTime() < _oper_cache.time_out()) ClearOperation(); //已经超时，清理缓存以及等待玩家操作的状态

	//如果不是放弃，才是当前玩家的操作
	if (Asset::PAI_OPER_TYPE_GIVEUP != pai_operate->oper_type())
	{
		_curr_player_index = player_index; //上面检查过，就说明当前该玩家可操作
		BroadCast(message); //广播玩家操作，玩家放弃操作不能广播
	}

	//const auto& pai = _oper_cache.pai(); //缓存的牌
	const auto& pai = pai_operate->pai(); //玩家发上来的牌

	//
	//一个人打牌之后，要检查其余每个玩家手中的牌，且等待他们的操作，直到超时
	//
	switch (pai_operate->oper_type())
	{
		case Asset::PAI_OPER_TYPE_DAPAI: //打牌
		case Asset::PAI_OPER_TYPE_TINGPAI: //听牌
		{
			//
			//加入牌池
			//
			Add2CardsPool(pai);

			//
			//(1) 检查各个玩家手里的牌是否满足胡、杠、碰、吃
			//
			//(2) 检查是否到了流局阶段
			//
			//(3) 否则给当前玩家的下家继续发牌
			//
			if (CheckPai(pai, player->GetID())) //有满足要求的玩家
			{
				SendCheckRtn();
			}
			else if (CheckLiuJu())
			{
				if (SendCheckRtn()) return;

				OnLiuJu();
			}
			else
			{
				ClearOperation();

				auto next_player_index = (_curr_player_index + 1) % MAX_PLAYER_COUNT; 

				auto player_next = GetPlayerByOrder(next_player_index);
				
				if (!player_next) return; 
				
				//DEBUG("player_id:{} next_player_id:{} _curr_player_index:{} next_player_index:{}", 
				//		player->GetID(), player_next->GetID(), _curr_player_index, next_player_index);

				auto cards = FaPai(1); 
				auto card = GameInstance.GetCard(cards[0]); //玩家待抓的牌

				if (player_next->HasTuoGuan()) _curr_player_index = next_player_index; //托管检查

				player_next->OnFaPai(cards); //放入玩家牌里面

				//if (player_next->HasTuoGuan()) return; //托管检查，防止递归
				
				Asset::PaiOperationAlert alert;

				//胡牌检查
				if (!player_next->IsJinbao() && player_next->CheckZiMo(false)) //自摸
				{
					auto pai_perator = alert.mutable_pais()->Add();
					pai_perator->mutable_pai()->CopyFrom(card);
					pai_perator->mutable_oper_list()->Add(Asset::PAI_OPER_TYPE_HUPAI);
						
					SetZiMoCache(player_next, card); //自摸胡牌缓存
				}
				//
				//玩家进行抓牌正好抓到宝胡
				//
				else if (player_next->CheckBaoHu(card)) //宝胡
				{
					auto pai_perator = alert.mutable_pais()->Add();
					pai_perator->mutable_pai()->CopyFrom(card);
					pai_perator->mutable_oper_list()->Add(Asset::PAI_OPER_TYPE_HUPAI);
					
					_oper_cache.set_player_id(player_next->GetID());
					_oper_cache.mutable_oper_list()->Add(Asset::PAI_OPER_TYPE_HUPAI);
					_oper_cache.mutable_pai()->CopyFrom(card);
				}

				//
				//听牌检查
				//
				std::vector<Asset::PaiElement> ting_list;
				if (player_next->CheckTingPai(ting_list))
				{
					_oper_cache.set_player_id(player_next->GetID());
					_oper_cache.mutable_oper_list()->Add(Asset::PAI_OPER_TYPE_TINGPAI);

					for (auto pai : ting_list) 
					{
						auto pai_perator = alert.mutable_pais()->Add();
						pai_perator->mutable_pai()->CopyFrom(pai);
						pai_perator->mutable_oper_list()->Add(Asset::PAI_OPER_TYPE_TINGPAI);
					
						_oper_cache.mutable_ting_pais()->Add()->CopyFrom(pai);
					}
				}
				
				//
				//明杠和暗杠检查
				//
				::google::protobuf::RepeatedField<Asset::PaiOperationAlert_AlertElement> gang_list;
				if (player_next->CheckAllGangPai(gang_list)) 
				{
					_oper_cache.set_player_id(player_next->GetID());

					for (auto gang : gang_list) 
					{
						auto pai_perator = alert.mutable_pais()->Add();
						pai_perator->CopyFrom(gang);
		
						_oper_cache.mutable_pai()->CopyFrom(gang.pai());
						for (auto oper_type : gang.oper_list()) _oper_cache.mutable_oper_list()->Add(Asset::PAI_OPER_TYPE(oper_type));
					}
				}
				
				//
				//旋风杠检查
				//
				auto xf_gang = player_next->CheckXuanFeng();
				if (xf_gang)
				{
					_oper_cache.set_player_id(player_next->GetID());

					auto pai_perator = alert.mutable_pais()->Add();
					pai_perator->mutable_oper_list()->Add((Asset::PAI_OPER_TYPE)xf_gang);
						
					_oper_cache.mutable_oper_list()->Add((Asset::PAI_OPER_TYPE)xf_gang);
				}

				if (alert.pais().size()) 
				{
					player_next->SendProtocol(alert); //提示Client

					_oper_cache.set_player_id(player_next->GetID()); //当前可操作玩家
					_oper_cache.set_source_player_id(player_next->GetID()); //当前牌来自玩家，自己抓牌
					_oper_cache.set_time_out(CommonTimerInstance.GetTime() + 30); //8秒后超时
				}
				else 
				{
					_curr_player_index = next_player_index;
				}
			}
		}
		break;
		
		case Asset::PAI_OPER_TYPE_HUPAI: //胡牌
		case Asset::PAI_OPER_TYPE_QIANGGANG: //抢杠胡
		{
			if (player->GetID() != _oper_cache.player_id())
			{
				LOG(ERROR, "玩家:{} 在房间:{} 牌局:{} 胡牌:{}，与服务器缓存:{}不一致，怀疑外挂行为，请关注", player->GetID(), _room_id, _game_id, pai.ShortDebugString(), _oper_cache.ShortDebugString());
			}

			if (player->CheckCardsInhand() && player->CheckHuPai(pai)) //玩家点炮
			{
				auto fan_list = player->GetFanList();

				if (player->IsJinbao()) 
				{
					fan_list.emplace(Asset::FAN_TYPE_JIN_BAO);

					_oper_cache.set_player_id(player->GetID()); //当前可操作玩家
					_oper_cache.set_source_player_id(player->GetID());
					_oper_cache.mutable_oper_list()->Add(Asset::PAI_OPER_TYPE_HUPAI);
					_oper_cache.mutable_pai()->CopyFrom(pai);

					_room->AddJinBao(player->GetID()); //进宝
				}

				Calculate(player->GetID(), _oper_cache.source_player_id(), fan_list); //结算
			}
			else if (player->CheckZiMo(pai)) 
			{
				auto fan_list = player->GetFanList();

				SetZiMoCache(player, pai); //自摸胡牌缓存

				if (player->IsJinbao()) 
				{
					fan_list.emplace(Asset::FAN_TYPE_JIN_BAO);
				}

				Calculate(player->GetID(), _oper_cache.source_player_id(), fan_list); //结算
			}
			else if (player->CheckBaoHu(pai) && player->HasPai(_baopai)) //宝胡
			{
				auto fan_list = player->GetFanList();

				fan_list.emplace(Asset::FAN_TYPE_LOU_BAO); 
					
				_oper_cache.set_player_id(player->GetID()); //当前可操作玩家
				_oper_cache.set_source_player_id(player->GetID());
				_oper_cache.mutable_oper_list()->Add(Asset::PAI_OPER_TYPE_HUPAI);
				_oper_cache.mutable_pai()->CopyFrom(_baopai);
					
				_room->AddLouBao(player->GetID()); //搂宝

				Calculate(player->GetID(), _oper_cache.source_player_id(), fan_list); //结算
			}
			else
			{
				player->PrintPai(); //打印玩家牌

				LOG(ERROR, "玩家:{}胡牌不满足条件，可能是外挂行为, 胡牌, 牌类型:{} 牌值:{}", player->GetID(), pai.card_type(), pai.card_value());
				
				player->AlertMessage(Asset::ERROR_GAME_PAI_UNSATISFIED); //没有牌满足条件

				auto player_next = GetNextPlayer(player->GetID());
				if (!player_next) return; 
				
				auto cards = FaPai(1); 
				player_next->OnFaPai(cards);
				
				_curr_player_index = (_curr_player_index + 1) % MAX_PLAYER_COUNT;
			}
		}
		break;
		
		case Asset::PAI_OPER_TYPE_GANGPAI: //杠牌
		case Asset::PAI_OPER_TYPE_ANGANGPAI: //杠牌
		{
			bool ret = player->CheckGangPai(pai, _oper_cache.source_player_id());
			if (!ret) 
			{
				player->AlertMessage(Asset::ERROR_GAME_PAI_UNSATISFIED); //没有牌满足条件
				return; 
			}
			else if (Asset::PAI_OPER_TYPE_GANGPAI == pai_operate->oper_type() && CheckQiangGang(pai, player->GetID()))
			{
				player->OnBeenQiangGang(pai, _oper_cache.source_player_id()); //删除牌和牌池处理
					
				auto source_player = GetPlayer(_oper_cache.source_player_id());
				if (source_player) source_player->CardsPoolPop(); //删除牌池

				SendCheckRtn();

				//_oper_list.clear(); //防止抢杠胡点过的情况，下家还能吃牌，从而会多牌
			}
			else
			{
				auto source_player_id = _oper_cache.source_player_id();

				ClearOperation(); //删除状态

				player->OnGangPai(pai, source_player_id);

				auto cards = TailPai(1); //从后楼给玩家取一张牌
				if (cards.size() == 0) return;

				player->OnFaPai(cards); //发牌

				Asset::PaiOperationAlert alert;
				//
				//旋风杠检查
				//
				auto gang = player->CheckXuanFeng();
				if (gang)
				{
					auto pai_perator = alert.mutable_pais()->Add();
					pai_perator->mutable_oper_list()->Add((Asset::PAI_OPER_TYPE)gang);
				}
				//
				//玩家杠牌检查
				//
				//杠检查(明杠和暗杠)
				//
				RepeatedField<Asset::PaiOperationAlert_AlertElement> gang_list;
				if (player->CheckAllGangPai(gang_list)) 
				{
					for (auto gang : gang_list) 
					{
						if (gang.pai().card_type() == pai.card_type() && gang.pai().card_value() == pai.card_value()) continue;

						auto pai_perator = alert.mutable_pais()->Add();
						pai_perator->CopyFrom(gang);
					
						_oper_cache.mutable_pai()->CopyFrom(gang.pai());
						_oper_cache.set_source_player_id(gang.pai().source_player_id());
						for (auto oper_type : gang.oper_list()) _oper_cache.mutable_oper_list()->Add(Asset::PAI_OPER_TYPE(oper_type));
					}
				}
				//
				//自摸检查
				//
				auto zhuapai = GameInstance.GetCard(cards[0]);
				if (player->CheckZiMo(false) || player->CheckBaoHu(zhuapai))
				{
					auto pai_perator = alert.mutable_pais()->Add();
					pai_perator->mutable_pai()->CopyFrom(zhuapai);
					pai_perator->mutable_oper_list()->Add(Asset::PAI_OPER_TYPE_HUPAI);
					
					SetZiMoCache(player, zhuapai); //自摸胡牌缓存
				}
				//
				//听牌检查
				//
				std::vector<Asset::PaiElement> pais;
				if (player->CheckTingPai(pais))
				{
					for (auto pai : pais) 
					{
						auto pai_perator = alert.mutable_pais()->Add();
						pai_perator->mutable_pai()->CopyFrom(pai);
						pai_perator->mutable_oper_list()->Add(Asset::PAI_OPER_TYPE_TINGPAI);
					}
				}
				
				if (alert.pais().size()) player->SendProtocol(alert); //提示Client

				_curr_player_index = GetPlayerOrder(player->GetID()); //设置当前玩家索引
				
				if (Asset::PAI_OPER_TYPE_GANGPAI == pai_operate->oper_type()) //明杠删除牌池
				{
					auto source_player = GetPlayer(source_player_id);
					if (source_player) source_player->CardsPoolPop();
				}
			}
		}
		break;

		case Asset::PAI_OPER_TYPE_PENGPAI: //碰牌
		{
			bool ret = player->CheckPengPai(pai);
			if (!ret) 
			{
				player->AlertMessage(Asset::ERROR_GAME_PAI_UNSATISFIED); //没有牌满足条件
				return; 
			}
			else
			{
				player->OnPengPai(pai);
				_curr_player_index = GetPlayerOrder(player->GetID()); //重置当前玩家索引

				auto source_player = GetPlayer(_oper_cache.source_player_id());
				ClearOperation(); //清理缓存以及等待玩家操作的状态

				Asset::PaiOperationAlert alert;
				//
				//听牌检查
				//
				std::vector<Asset::PaiElement> pais;
				if (player->CheckTingPai(pais)) 
				{
					_oper_cache.mutable_oper_list()->Add(Asset::PAI_OPER_TYPE_TINGPAI);
					_oper_cache.set_player_id(player->GetID());

					for (auto pai : pais) 
					{
						auto pai_perator = alert.mutable_pais()->Add();
						pai_perator->mutable_pai()->CopyFrom(pai);
						pai_perator->mutable_oper_list()->Add(Asset::PAI_OPER_TYPE_TINGPAI);

						_oper_cache.mutable_ting_pais()->Add()->CopyFrom(pai);
					}
				}
				//
				//旋风杠检查
				//
				auto xuanfeng_gang = player->CheckXuanFeng();
				if (xuanfeng_gang)
				{
					auto pai_perator = alert.mutable_pais()->Add();
					pai_perator->mutable_oper_list()->Add((Asset::PAI_OPER_TYPE)xuanfeng_gang);
					
					_oper_cache.set_player_id(player->GetID());
					_oper_cache.mutable_oper_list()->Add((Asset::PAI_OPER_TYPE)xuanfeng_gang);
				}
				//
				//玩家杠牌检查
				//
				//杠检查(明杠和暗杠)
				//
				RepeatedField<Asset::PaiOperationAlert_AlertElement> gang_list;
				if (player->CheckAllGangPai(gang_list)) 
				{
					_oper_cache.set_player_id(player->GetID());

					for (auto gang : gang_list) 
					{
						if (gang.pai().card_type() == pai.card_type() && gang.pai().card_value() == pai.card_value()) continue;

						auto pai_perator = alert.mutable_pais()->Add();
						pai_perator->CopyFrom(gang);
					
						_oper_cache.mutable_pai()->CopyFrom(gang.pai());
						for (auto oper_type : gang.oper_list()) _oper_cache.mutable_oper_list()->Add(Asset::PAI_OPER_TYPE(oper_type));
					}
				}

				if (alert.pais().size()) player->SendProtocol(alert); //提示Client
				
				if (source_player) source_player->CardsPoolPop();
			}
		}
		break;

		case Asset::PAI_OPER_TYPE_CHIPAI: //吃牌
		{
			bool ret = player->CheckChiPai(pai);
			if (!ret) 
			{
				player->AlertMessage(Asset::ERROR_GAME_PAI_UNSATISFIED); //没有牌满足条件
				return; 
			}
			else
			{
				player->OnChiPai(pai, message);

				auto source_player = GetPlayer(_oper_cache.source_player_id());
				ClearOperation(); //清理缓存以及等待玩家操作的状态

				Asset::PaiOperationAlert alert;
				//
				//听牌检查
				//
				std::vector<Asset::PaiElement> pais;
				if (player->CheckTingPai(pais)) 
				{
					_oper_cache.set_player_id(player->GetID());
					_oper_cache.mutable_oper_list()->Add(Asset::PAI_OPER_TYPE_TINGPAI);

					for (auto pai : pais) 
					{
						auto pai_perator = alert.mutable_pais()->Add();
						pai_perator->mutable_pai()->CopyFrom(pai);
						pai_perator->mutable_oper_list()->Add(Asset::PAI_OPER_TYPE_TINGPAI);

						_oper_cache.mutable_ting_pais()->Add()->CopyFrom(pai);
					}
				}
				//
				//旋风杠检查
				//
				auto xuanfeng_gang = player->CheckXuanFeng();
				if (xuanfeng_gang)
				{
					auto pai_perator = alert.mutable_pais()->Add();
					pai_perator->mutable_oper_list()->Add((Asset::PAI_OPER_TYPE)xuanfeng_gang);
					
					_oper_cache.set_player_id(player->GetID());
					_oper_cache.mutable_oper_list()->Add((Asset::PAI_OPER_TYPE)xuanfeng_gang);
				}
				//
				//玩家杠牌检查
				//
				//杠检查(明杠和暗杠)
				//
				RepeatedField<Asset::PaiOperationAlert_AlertElement> gang_list;
				if (player->CheckAllGangPai(gang_list)) 
				{
					_oper_cache.set_player_id(player->GetID());

					for (auto gang : gang_list) 
					{
						if (gang.pai().card_type() == pai.card_type() && gang.pai().card_value() == pai.card_value()) continue;

						auto pai_perator = alert.mutable_pais()->Add();
						pai_perator->CopyFrom(gang);
					
						_oper_cache.mutable_pai()->CopyFrom(gang.pai());
						for (auto oper_type : gang.oper_list()) _oper_cache.mutable_oper_list()->Add(Asset::PAI_OPER_TYPE(oper_type));
					}
				}
					
				if (alert.pais().size()) player->SendProtocol(alert); //提示Client
				
				if (source_player) source_player->CardsPoolPop();
			}
		}
		break;
		
		case Asset::PAI_OPER_TYPE_GIVEUP: //放弃
		{
			if (IsQiangGang(player->GetID())) //抢杠不胡
			{
				auto player = GetPlayer(_oper_cache.source_player_id());
				if (!player) return;

				player->OnBeenQiangGangWithGivingUp(_oper_cache.pai(), _oper_cache.source_player_id()); //TODO: 建平抢杠胡点过，点杠会转成过杠，_oper_cache.source_player_id() 不是打出杠牌的玩家，而是杠牌玩家
				
				ClearOperation(); //缓存清理

				auto cards = TailPai(1); //从后楼给玩家取一张牌
				if (cards.size() == 0) return;

				player->OnFaPai(cards); //发牌

				Asset::PaiOperationAlert alert;
				//
				//旋风杠检查
				//
				auto gang = player->CheckXuanFeng();
				if (gang)
				{
					auto pai_perator = alert.mutable_pais()->Add();
					pai_perator->mutable_oper_list()->Add((Asset::PAI_OPER_TYPE)gang);
				}
				//
				//玩家杠牌检查
				//
				//杠检查(明杠和暗杠)
				//
				RepeatedField<Asset::PaiOperationAlert_AlertElement> gang_list;
				if (player->CheckAllGangPai(gang_list)) 
				{
					_oper_cache.set_player_id(player->GetID());

					for (auto gang : gang_list) 
					{
						if (gang.pai().card_type() == pai.card_type() && gang.pai().card_value() == pai.card_value()) continue;

						auto pai_perator = alert.mutable_pais()->Add();
						pai_perator->CopyFrom(gang);
					
						_oper_cache.mutable_pai()->CopyFrom(gang.pai());
						for (auto oper_type : gang.oper_list()) _oper_cache.mutable_oper_list()->Add(Asset::PAI_OPER_TYPE(oper_type));
					}
				}
				//
				//自摸检查
				//
				auto zhuapai = GameInstance.GetCard(cards[0]);
				if (player->CheckZiMo(false) || player->CheckBaoHu(zhuapai))
				{
					auto pai_perator = alert.mutable_pais()->Add();
					pai_perator->mutable_pai()->CopyFrom(zhuapai);
					pai_perator->mutable_oper_list()->Add(Asset::PAI_OPER_TYPE_HUPAI);
					
					SetZiMoCache(player, zhuapai); //自摸胡牌缓存
				}
				//
				//听牌检查
				//
				std::vector<Asset::PaiElement> pais;
				if (player->CheckTingPai(pais))
				{
					for (auto pai : pais) 
					{
						auto pai_perator = alert.mutable_pais()->Add();
						pai_perator->mutable_pai()->CopyFrom(pai);
						pai_perator->mutable_oper_list()->Add(Asset::PAI_OPER_TYPE_TINGPAI);
					}
				}
				
				if (alert.pais().size()) player->SendProtocol(alert); //提示Client

				_curr_player_index = GetPlayerOrder(player->GetID()); //设置当前玩家索引
				return;
			}
			else if (SendCheckRtn()) 
			{
				return;
			}
			else if (IsLiuJu())
			{
				OnLiuJu();
				return;
			}
			else if (CheckLiuJu())
			{
				if (SendCheckRtn()) return;

				OnLiuJu();
				return;
			}
	
			auto next_player_index = (_curr_player_index + 1) % MAX_PLAYER_COUNT; //如果有玩家放弃操作，则继续下个玩家

			auto player_next = GetPlayerByOrder(next_player_index);
			if (!player_next) return; 
			
			if (player->GetID() == player_next->GetID() && !player_next->CheckCardsInhand()) 
			{
				ERROR("玩家:{}放弃操作，当前牌数量不能再次发牌", player->GetID());

				_curr_player_index = next_player_index;
				return;
			}
			
			DEBUG("oper_limit.player_id:{} player_id:{} next_player_id:{} _curr_player_index:{} next_player_index:{}",
				_oper_cache.player_id(), player->GetID(), player_next->GetID(), _curr_player_index, next_player_index);

			auto cards = FaPai(1); 
			auto card = GameInstance.GetCard(cards[0]); //玩家待抓的牌
			
			player_next->OnFaPai(cards); //放入玩家牌里面

			Asset::PaiOperationAlert alert;

			//
			//胡牌检查
			//
			if (player_next->CheckZiMo(false)) //自摸检查
			{
				auto pai_perator = alert.mutable_pais()->Add();
				pai_perator->mutable_pai()->CopyFrom(card);
				pai_perator->mutable_oper_list()->Add(Asset::PAI_OPER_TYPE_HUPAI);
				
				SetZiMoCache(player_next, card); //自摸胡牌缓存
			}

			//
			//玩家摸宝之后进行抓牌正好抓到宝胡
			//
			else if (player_next->CheckBaoHu(card)) //宝胡
			{
				auto pai_perator = alert.mutable_pais()->Add();
				pai_perator->mutable_pai()->CopyFrom(card);
				pai_perator->mutable_oper_list()->Add(Asset::PAI_OPER_TYPE_HUPAI);
				
				SetZiMoCache(player_next, card); //自摸胡牌缓存
			}
			//	
			//听牌检查
			//
			std::vector<Asset::PaiElement> ting_list;
			if (player_next->CheckTingPai(ting_list)) 
			{
				_oper_cache.set_player_id(player_next->GetID());
				_oper_cache.mutable_oper_list()->Add(Asset::PAI_OPER_TYPE_TINGPAI);

				for (auto pai : ting_list) 
				{
					auto pai_perator = alert.mutable_pais()->Add();
					pai_perator->mutable_pai()->CopyFrom(pai);
					pai_perator->mutable_oper_list()->Add(Asset::PAI_OPER_TYPE_TINGPAI);

					_oper_cache.mutable_ting_pais()->Add()->CopyFrom(pai);
				}
			}
			//	
			//杠检查(明杠和暗杠)
			//
			::google::protobuf::RepeatedField<Asset::PaiOperationAlert_AlertElement> gang_list;
			if (player_next->CheckAllGangPai(gang_list)) 
			{
				_oper_cache.set_player_id(player_next->GetID());

				for (auto gang : gang_list) 
				{
					auto pai_perator = alert.mutable_pais()->Add();
					pai_perator->CopyFrom(gang);
						
					_oper_cache.mutable_pai()->CopyFrom(gang.pai());
					for (auto oper_type : gang.oper_list()) _oper_cache.mutable_oper_list()->Add(Asset::PAI_OPER_TYPE(oper_type));
				}
			}
			//
			//旋风杠检查
			//
			//开局状态，上家打牌，其他玩家可碰，但放弃，此时需要检查当前玩家是否可以旋风杠
			//
			auto xf_gang = player_next->CheckXuanFeng();
			if (xf_gang)
			{
				auto pai_perator = alert.mutable_pais()->Add();
				pai_perator->mutable_oper_list()->Add((Asset::PAI_OPER_TYPE)xf_gang);
					
				_oper_cache.set_player_id(player_next->GetID());
				_oper_cache.mutable_oper_list()->Add((Asset::PAI_OPER_TYPE)xf_gang);
			}
			//
			//开局状态，当前玩家拥有中发白白，上家打了白板
			//
			//当前玩家选择放弃，此时要提示当前玩家旋风杠.
			//
			if (_oper_cache.player_id() == player_next->GetID() || player->GetID() == player_next->GetID()/*当前操作玩家还是自己*/) //旋风杠检查
			{
				/*
				auto xf_gang = player->CheckXuanFeng();
				if (xf_gang)
				{
					auto pai_perator = alert.mutable_pais()->Add();
					pai_perator->mutable_oper_list()->Add((Asset::PAI_OPER_TYPE)xf_gang);
				}
				*/

				if (alert.pais().size()) 
				{
					player_next->SendProtocol(alert); //提示Client

					_oper_cache.set_player_id(player_next->GetID()); //当前可操作玩家
					_oper_cache.set_source_player_id((player_next->GetID())); //当前牌来自玩家，自己抓牌，所以是自己
					_oper_cache.set_time_out(CommonTimerInstance.GetTime() + 30); //8秒后超时
				}
				else
				{
					_curr_player_index = next_player_index;
					ClearOperation(); //清理缓存以及等待玩家操作的状态
				}
			}
			else if (alert.pais().size()) 
			{
				player_next->SendProtocol(alert); //提示Client

				_oper_cache.set_player_id(player_next->GetID()); //当前可操作玩家
				_oper_cache.set_source_player_id((player_next->GetID())); //当前牌来自玩家，自己抓牌，所以是自己
				_oper_cache.set_time_out(CommonTimerInstance.GetTime() + 30); //8秒后超时
			}
			else 
			{
				_curr_player_index = next_player_index;
			}
		}
		break;

		default:
		{
			//DEBUG("%s:line:%d 服务器接收未能处理的协议，玩家角色:player_id:%ld, 操作类型:%d\n", __func__, __LINE__, player->GetID(), pai_operate->oper_type());
			//DEBUG_ASSERT(false);
			return; //直接退出
		}
		break;
	}
}

void Game::ClearOperation()
{
	DEBUG("房间:{} 牌局:{} 清理缓存状态:{}", _room_id, _game_id, _oper_cache.ShortDebugString());

	_oper_cache.Clear(); //清理状态
}
	
bool Game::SanJiaBi(int64_t hupai_player_id)
{
	for (int i = 0; i < MAX_PLAYER_COUNT; ++i)
	{
		auto player = _players[i];
		if (!player || player->GetID() == hupai_player_id) continue;
		if (!player->IsBimen()) return false;
	}

	return true;
}
	
void Game::PaiPushDown()
{
	Asset::PaiPushDown proto;

	for (int i = 0; i < MAX_PLAYER_COUNT; ++i)
	{
		auto player = _players[i];
		if (!player) 
		{
			ERROR("player_index:{} has not found, maybe it has disconneced.", i);
			continue;
		}

		auto player_info = proto.mutable_player_list()->Add();
		player_info->set_player_id(player->GetID());
		player_info->set_position(player->GetPosition());

		const auto& cards = player->GetCardsInhand();

		for (auto it = cards.begin(); it != cards.end(); ++it)
		{
			auto pai = player_info->mutable_pai_list()->Add();

			pai->set_card_type((Asset::CARD_TYPE)it->first); //牌类型

			for (auto card_value : it->second)
			{
				pai->mutable_cards()->Add(card_value); //牌值
			}
		}
	}

	BroadCast(proto);
}
	
void Game::Calculate(int64_t hupai_player_id/*胡牌玩家*/, int64_t dianpao_player_id/*点炮玩家*/, std::unordered_set<int32_t>& fan_list)
{
	if (!_room) return;

	//DEBUG("玩家胡牌, 胡牌玩家:{} 点炮玩家:{}", hupai_player_id, dianpao_player_id);

	//
	//1.推到牌
	//
	PaiPushDown();

	//
	//2.结算
	//
	if (hupai_player_id != dianpao_player_id) _room->AddDianpao(dianpao_player_id); //不是自摸
	
	int32_t base_score = 1;

	//
	//玩家角色性检查(比如，庄家胡牌)
	//
	if (IsBanker(hupai_player_id)) 
	{
		fan_list.emplace(Asset::FAN_TYPE_ZHUANG);
	}

	auto hu_player = GetPlayer(hupai_player_id);
	if (!hu_player) return;

	if (dianpao_player_id == hupai_player_id && hu_player->IsGangOperation()) //杠上开
	{
		fan_list.emplace(Asset::FAN_TYPE_GANG_SHANG_KAI);
	}

	Asset::GameCalculate message;
	message.set_calculte_type(Asset::CALCULATE_TYPE_HUPAI);
	message.mutable_baopai()->CopyFrom(_baopai);

	auto dianpao_player = GetPlayer(dianpao_player_id);
	if (dianpao_player) message.set_dianpao_player_position(dianpao_player->GetPosition());
	//
	//胡牌积分，三部分
	//
	//1.各个玩家输牌积分
	//
	
	int32_t top_mutiple = _room->MaxFan(); //封顶番数

	for (int i = 0; i < MAX_PLAYER_COUNT; ++i)
	{
		auto player = _players[i];
		if (!player) return;
		
		auto player_id = player->GetID();
		
		auto record = message.mutable_record()->mutable_list()->Add();
		record->set_player_id(player_id);
		record->set_nickname(player->GetNickName());
		record->set_headimgurl(player->GetHeadImag());

		if (hupai_player_id == player_id) continue;

		int32_t score = base_score;
		
		//
		//牌型基础分值计算
		//
		for (const auto& fan : fan_list)
		{
			score *= GetMultiple(fan);
			
			auto detail = record->mutable_details()->Add();
			detail->set_fan_type((Asset::FAN_TYPE)fan);
			detail->set_score(-score);

			//DEBUG("player_id:{} fan:{} score:{}", player_id, fan, -score);
		}
	
		//
		//操作和玩家牌状态分值计算
		//
		//每个玩家不同
		//
			
		if (dianpao_player_id == hupai_player_id)
		{
			score *= GetMultiple(Asset::FAN_TYPE_ZI_MO); //自摸
			
			auto detail = record->mutable_details()->Add();
			detail->set_fan_type(Asset::FAN_TYPE_ZI_MO);
			detail->set_score(-score);
			
			//DEBUG("player_id:{} fan:{} score:{}", player_id, Asset::FAN_TYPE_ZI_MO, -score);
		}

		if (dianpao_player_id != hupai_player_id && player_id == dianpao_player_id) 
		{
			score *= GetMultiple(Asset::FAN_TYPE_DIAN_PAO); 
			
			auto detail = record->mutable_details()->Add();
			detail->set_fan_type(Asset::FAN_TYPE_DIAN_PAO);
			detail->set_score(-score);

			//
			//杠后流泪
			//
			//胡别人补杠后打出的牌
			//
			if (player->IsGangOperation()) //流泪
			{
				score *= GetMultiple(Asset::FAN_TYPE_LIU_LEI); 
				
				auto detail = record->mutable_details()->Add();
				detail->set_fan_type(Asset::FAN_TYPE_LIU_LEI);
				detail->set_score(-score);
			}
			
			//DEBUG("player_id:{} fan:{} score:{}", player_id, Asset::FAN_TYPE_DIAN_PAO, -score);
		}
		
		//
		//庄家
		//
		if (IsBanker(player_id)) 
		{
			score *= GetMultiple(Asset::FAN_TYPE_ZHUANG); 
			
			auto detail = record->mutable_details()->Add();
			detail->set_fan_type(Asset::FAN_TYPE_ZHUANG);
			detail->set_score(-score);
		}

		if (player->IsBimen()) 
		{
			score *= GetMultiple(Asset::FAN_TYPE_BIMEN); //闭门翻番
			
			auto detail = record->mutable_details()->Add();
			detail->set_fan_type(Asset::FAN_TYPE_BIMEN);
			detail->set_score(-score);
			
			//DEBUG("player_id:{} fan:{} score:{}", player_id, Asset::FAN_TYPE_BIMEN, -score);
		}
		
		if (player->IsMingPiao()) 
		{
			score *= GetMultiple(Asset::FAN_TYPE_PIAO_WEIHU); //明飘未胡
			
			auto detail = record->mutable_details()->Add();
			detail->set_fan_type(Asset::FAN_TYPE_PIAO_WEIHU);
			detail->set_score(-score);
			
			//DEBUG("player_id:{} fan:{} score:{}", player_id, Asset::FAN_TYPE_PIAO_WEIHU, -score);
		}
		
		if (SanJiaBi(hupai_player_id)) 
		{
			score *= GetMultiple(Asset::FAN_TYPE_SAN_JIA_BI_MEN); //三家闭门
			
			auto detail = record->mutable_details()->Add();
			detail->set_fan_type(Asset::FAN_TYPE_SAN_JIA_BI_MEN);
			detail->set_score(-score);
			
			//DEBUG("player_id:{} fan:{} score:{}", player_id, Asset::FAN_TYPE_SAN_JIA_BI_MEN, -score);
		}

		//
		//输牌玩家番数上限封底
		//
		if (top_mutiple > 0) score = std::min(top_mutiple, score); //封顶

		record->set_score(-score); //玩家所输积分
			
		//DEBUG("玩家:{} 因为牌型和位置输所有积分:{}", player_id, -score);
	}

	DEBUG("胡牌测试:{}", message.ShortDebugString());
	
	//
	//2.胡牌玩家积分
	//
	//其他玩家积分之和
	//
	auto get_record = [&](int64_t player_id)->google::protobuf::internal::RepeatedPtrIterator<Adoter::Asset::GameRecord_GameElement> { 
		auto it = std::find_if(message.mutable_record()->mutable_list()->begin(), message.mutable_record()->mutable_list()->end(), 
				[player_id](const Asset::GameRecord_GameElement& ele){
					return player_id == ele.player_id();
			});
		return it;
	};
	
	auto record = get_record(hupai_player_id); 
	if (record == message.mutable_record()->mutable_list()->end()) return;

	auto total_score = 0;
	std::unordered_map<int64_t, int32_t> player_score; //各个玩家输积分//不包括杠

	for (int32_t i = 0; i < message.record().list().size(); ++i)
	{
		const auto& list_element = message.record().list(i);

		if (list_element.player_id() == hupai_player_id) continue;

		if (_room->HasYiJiaFu() && dianpao_player_id != 0 && dianpao_player_id != hupai_player_id && dianpao_player_id != list_element.player_id()) 
		{
			message.mutable_record()->mutable_list(i)->set_score(0); //点炮一家付//其他两家不必付钱
			message.mutable_record()->mutable_list(i)->mutable_details()->Clear();
			continue; 
		}

		total_score -= list_element.score();
		player_score.emplace(list_element.player_id(), -list_element.score());
		
		for (const auto& element : list_element.details())
		{
			const auto& fan_type = element.fan_type();

			auto it_score = std::find_if(record->mutable_details()->begin(), record->mutable_details()->end(), 
				[fan_type](const Asset::GameRecord_GameElement_DetailElement& detail_element){
					return detail_element.fan_type() == fan_type;
			});
			if (it_score == record->mutable_details()->end()) 
			{
				auto rcd = record->mutable_details()->Add();
				rcd->set_fan_type(element.fan_type());
				rcd->set_score(-element.score());
			}
			else
			{
				it_score->set_score(it_score->score() + (-element.score())); //输牌玩家存储的是负数
			}
		}
	}

	record->set_score(total_score); //胡牌玩家赢的总积分
	
	DEBUG("胡牌测试:{}", message.ShortDebugString());
	
	//
	//3.杠牌积分
	//
	CalculateGangScore(message);
	
	DEBUG("胡牌测试:{}", message.ShortDebugString());
	
	/*
	for (int i = 0; i < MAX_PLAYER_COUNT; ++i)
	{
		auto player = _players[i];
		if (!player) return;

		auto minggang = player->GetMingGang();
		auto ming_count = player->GetMingGangCount(); 
		auto an_count = player->GetAnGangCount(); 
		auto xf_count = player->GetXuanFengCount(); 
		
		int32_t ming_score = ming_count * GetMultiple(Asset::FAN_TYPE_MING_GANG);
		int32_t an_score = an_count * GetMultiple(Asset::FAN_TYPE_AN_GANG);
		int32_t xf_score = xf_count * GetMultiple(Asset::FAN_TYPE_XUAN_FENG_GANG);

		int32_t score = ming_score + an_score + xf_score; //玩家杠牌赢得其他单个玩家积分

		auto record = message.mutable_record()->mutable_list(i);
		record->set_score(record->score() + score * (MAX_PLAYER_COUNT - 1)); //增加杠牌玩家总杠积分

		//
		//1.杠牌玩家赢得积分列表
		//
		if (ming_count)
		{
			auto detail = record->mutable_details()->Add();
			detail->set_fan_type(Asset::FAN_TYPE_MING_GANG);
			detail->set_score(ming_score * (MAX_PLAYER_COUNT - 1));
		}

		if (an_count)
		{
			auto detail = record->mutable_details()->Add();
			detail->set_fan_type(Asset::FAN_TYPE_AN_GANG);
			detail->set_score(an_score * (MAX_PLAYER_COUNT - 1));
		}
		
		if (xf_count)
		{
			auto detail = record->mutable_details()->Add();
			detail->set_fan_type(Asset::FAN_TYPE_XUAN_FENG_GANG);
			detail->set_score(xf_score * (MAX_PLAYER_COUNT - 1));
		}
		//
		//2.其他玩家所输积分
		//
		for (int index = 0; index < MAX_PLAYER_COUNT; ++index)
		{
			if (index == i) continue;

			auto record = message.mutable_record()->mutable_list(index);
			record->set_score(record->score() - score); //扣除杠分

			//非杠牌玩家所输积分列表

			if (ming_count) //明杠，根据点杠玩家不同
			{
				auto detail = record->mutable_details()->Add();
				detail->set_fan_type(Asset::FAN_TYPE_MING_GANG);

				int32_t single_score = GetMultiple(Asset::FAN_TYPE_MING_GANG);

				for (auto gang : minggang)
				{
					if (_room->IsJianPing() && player->GetID() == gang.source_player_id()) //点杠：建平玩法
					{ 
						detail->set_score(detail->score() - single_score * (MAX_PLAYER_COUNT - 1)); //点杠的那个人出3分
					}
					else 
					{ 
						detail->set_score(detail->score() - single_score); 
					}
				}
			}

			if (an_count)
			{
				auto detail = record->mutable_details()->Add();
				detail->set_fan_type(Asset::FAN_TYPE_AN_GANG);
				detail->set_score(-an_score);
			}

			if (xf_count)
			{
				auto detail = record->mutable_details()->Add();
				detail->set_fan_type(Asset::FAN_TYPE_XUAN_FENG_GANG);
				detail->set_score(-xf_score);
			}
		} 
	} 
	*/
	
	//
	//4.点炮赔付
	//
	//(1) 点炮包三家
	//
	//除了杠分都由点炮玩家付
	//
	//(2) 点炮一家付
	//		
	//谁点谁给自己的点炮钱(不用帮其他两家给)，其他两家不掏钱
	//
	//别人杠要帮付，点杠不帮付
	//
	if ((_room->HasBaoSanJia() || _room->HasYiJiaFu()) && dianpao_player_id != 0 && dianpao_player_id != hupai_player_id) //杠不需要赔付
	{
		auto it_dianpao = get_record(dianpao_player_id);
		if (it_dianpao == message.mutable_record()->mutable_list()->end()) return;

		int32_t baofen_total = 0; //包积分，实则包赔两个玩家的积分

		for (auto player : _players)
		{
			if (!player) continue;

			auto player_id = player->GetID();
			if (player_id == hupai_player_id || player_id == dianpao_player_id) continue; //和胡牌玩家无关

			auto it_player = get_record(player_id);
			if (it_player == message.mutable_record()->mutable_list()->end()) continue;

			it_player->set_score(it_player->score() + player_score[player_id]); //返回积分
			baofen_total += player_score[player_id]; 
		}
			
		auto detail = it_dianpao->mutable_details()->Add();
		detail->set_fan_type(Asset::FAN_TYPE_BAOSANJIA);
		detail->set_score(-baofen_total);

		it_dianpao->set_score(-baofen_total + it_dianpao->score()); //总积分
	}
	
	DEBUG("胡牌测试:{}", message.ShortDebugString());
	
	//
	//如果仅仅是平胡，则显示
	//
	for (int32_t i = 0; i < message.record().list().size(); ++i)
	{
		bool is_pinghu = true;

		if (message.record().list(i).details().size() == 0)
		{
			is_pinghu = true;
		}
		else
		{
			for (int32_t j = 0; j < message.record().list(i).details().size(); ++j)
			{
				if (message.record().list(i).details(j).fan_type() != Asset::FAN_TYPE_MING_GANG 
						&& message.record().list(i).details(j).fan_type() != Asset::FAN_TYPE_AN_GANG
						&& message.record().list(i).details(j).fan_type() != Asset::FAN_TYPE_XUAN_FENG_GANG) //只有杠是平胡
				{
					is_pinghu = false;
					break;
				}
			}
		}

		if (is_pinghu && !_room->HasYiJiaFu()) //一家付不显示任何番数
		{
			auto pinghu = message.mutable_record()->mutable_list(i)->mutable_details()->Add();
			pinghu->set_score(1);
			pinghu->set_fan_type(Asset::FAN_TYPE_PINGHU); 
		}
	}
	
	//
	//最大番数
	//
	auto max_fan_it = std::max_element(record->details().begin(), record->details().end(), 
			[&](const Asset::GameRecord_GameElement_DetailElement& detail1, const Asset::GameRecord_GameElement_DetailElement& detail2){
				return GetMultiple(detail1.fan_type()) < GetMultiple(detail2.fan_type()) ;
			});
	if (max_fan_it != record->details().end()) message.set_max_fan_type(max_fan_it->fan_type());

	//
	//好友房//匹配房记录消耗
	//
	auto room_type = _room->GetType();

	if (Asset::ROOM_TYPE_FRIEND == room_type)   
	{
		_room->AddGameRecord(message.record()); //本局记录
	}
	else
	{
		const auto& messages = AssetInstance.GetMessagesByType(Asset::ASSET_TYPE_ROOM);

		auto it = std::find_if(messages.begin(), messages.end(), [room_type](pb::Message* message){
			auto room_limit = dynamic_cast<Asset::RoomLimit*>(message);
			if (!room_limit) return false;

			return room_type == room_limit->room_type();
		});

		if (it == messages.end()) return;
		
		auto room_limit = dynamic_cast<Asset::RoomLimit*>(*it);
		if (!room_limit) return;

		for (auto player : _players)
		{
			if (!player) continue;

			auto record = get_record(player->GetID()); 
			if (record == message.mutable_record()->mutable_list()->end()) continue;
			
			auto total_score = record->score();
			auto consume_count = total_score * room_limit->base_count();
			
			auto beans_count = player->GetHuanledou();
			if (beans_count < consume_count) consume_count = beans_count; //如果玩家欢乐豆不足，则扣成0

			auto consume_real = player->ConsumeHuanledou(Asset::HUANLEDOU_CHANGED_TYPE_GAME, consume_count); //消耗
			if (consume_count != consume_real) continue;
		}
	}
	
	DEBUG("胡牌测试:{}", message.ShortDebugString());
	
	BroadCast(message);
	OnGameOver(hupai_player_id); //结算之后才是真正结束
	
	auto room_id = _room->GetID();
	auto curr_count = _room->GetGamesCount();
	auto open_rands = _room->GetOpenRands();
	auto message_string = message.ShortDebugString();

	LOG(INFO, "房间:{}第:{}/{}局结束，胡牌玩家:{} 点炮玩家:{}, 胡牌结算:{}", room_id, curr_count, open_rands, hupai_player_id, dianpao_player_id, message_string);
}
	
void Game::BroadCast(pb::Message* message, int64_t exclude_player_id)
{
	if (!_room) return;

	_room->BroadCast(message, exclude_player_id);
}

void Game::Add2CardsPool(Asset::PaiElement pai) 
{ 
	_cards_pool.push_back(pai); 
}

void Game::Add2CardsPool(Asset::CARD_TYPE card_type, int32_t card_value) 
{
	Asset::PaiElement pai;
	pai.set_card_type(card_type);
	pai.set_card_value(card_value);

	Add2CardsPool(pai);
}

void Game::BroadCast(pb::Message& message, int64_t exclude_player_id)
{
	if (!_room) return;

	_room->BroadCast(message, exclude_player_id);
}

bool Game::SendCheckRtn()
{
	ClearOperation();

	if (_oper_list.size() == 0) return false;

	auto check = [this](Asset::PAI_OPER_TYPE rtn_type, Asset::PaiOperationCache& operation)->bool {

		for (const auto& oper : _oper_list)
		{
			auto it = std::find(oper.oper_list().begin(), oper.oper_list().end(), rtn_type);

			if (it != oper.oper_list().end()) 
			{
				operation = oper;

				return true;
			}
		}
		return false;
	};

	Asset::PaiOperationCache operation;
	for (int32_t i = Asset::PAI_OPER_TYPE_HUPAI; i <= Asset::PAI_OPER_TYPE_COUNT; ++i)
	{
		auto result = check((Asset::PAI_OPER_TYPE)i, operation);
		if (result) break;
	}

	if (operation.oper_list().size() == 0) return false;

	int64_t player_id = operation.player_id(); 

	_oper_cache.set_player_id(player_id); //当前可操作玩家
	_oper_cache.set_source_player_id(operation.source_player_id()); //当前牌来自玩家
	_oper_cache.mutable_pai()->CopyFrom(operation.pai()); //缓存这张牌
	_oper_cache.set_time_out(CommonTimerInstance.GetTime() + 30); //8秒后超时
	
	Asset::PaiOperationAlert alert;

	auto pai_perator = alert.mutable_pais()->Add();
	pai_perator->mutable_pai()->CopyFrom(operation.pai());

	for (auto rtn : operation.oper_list()) 
	{
		pai_perator->mutable_oper_list()->Add(rtn); //可操作牌类型
		_oper_cache.mutable_oper_list()->Add(rtn); //缓存操作
	}

	if (auto player_to = GetPlayer(player_id)) 
		player_to->SendProtocol(alert); //发给目标玩家

	auto it = std::find_if(_oper_list.begin(), _oper_list.end(), [player_id](const Asset::PaiOperationCache& operation) {
		return player_id == operation.player_id();
	});

	if (it != _oper_list.end()) _oper_list.erase(it); //删除第一个满足条件的数据

	return true;
}
	
/////////////////////////////////////////////////////
//
//检查各个玩家能否对该牌进行操作
//
//返回可操作玩家的索引
//
/////////////////////////////////////////////////////

bool Game::CheckPai(const Asset::PaiElement& pai, int64_t source_player_id)
{
	_oper_list.clear();

	int32_t player_index = GetPlayerOrder(source_player_id); //当前玩家索引
	if (player_index == -1) 
	{
		//DEBUG_ASSERT(false);
		return false; //理论上不会出现
	}

	//assert(_curr_player_index == player_index); //理论上一定相同：错误，如果碰牌的玩家出牌就不一定

	int32_t next_player_index = (_curr_player_index + 1) % MAX_PLAYER_COUNT;

	for (int32_t i = next_player_index; i < MAX_PLAYER_COUNT - 1 + next_player_index; ++i)
	{
		auto cur_index = i % MAX_PLAYER_COUNT;

		auto player = GetPlayerByOrder(cur_index);
		if (!player) 
		{
			//DEBUG_ASSERT(false);
			return false; //理论上不会出现
		}

		if (source_player_id == player->GetID()) continue; //自己不能对自己的牌进行操作

		auto rtn_check = player->CheckPai(pai, source_player_id); //不能包括宝胡
		if (rtn_check.size() == 0) continue; //不能吃、碰、杠和胡牌

		//for (auto value : rtn_check)
		//	DEBUG("operation player can do: cur_player_index:{} next_player_index:{} player_id:{} value:{}", cur_index, next_player_index, player->GetID(), value);
		
		auto it_chi = std::find(rtn_check.begin(), rtn_check.end(), Asset::PAI_OPER_TYPE_CHIPAI);
		if (it_chi != rtn_check.end() && cur_index != next_player_index) rtn_check.erase(it_chi); //只有下家能吃牌
		
		if (rtn_check.size() == 0) continue; 

		//
		//缓存所有操作
		//
		Asset::PaiOperationCache pai_operation;
		pai_operation.set_player_id(player->GetID());
		pai_operation.set_source_player_id(source_player_id);
		pai_operation.mutable_pai()->CopyFrom(pai);

		for (auto result : rtn_check) pai_operation.mutable_oper_list()->Add(result);
		_oper_list.push_back(pai_operation);
	}

	return _oper_list.size() > 0;
}

bool Game::IsQiangGang(int64_t player_id)
{
	if (_oper_cache.player_id() != player_id) return false;
	if (_oper_cache.source_player_id() == 0) return false;

	auto it = std::find(_oper_cache.oper_list().begin(), _oper_cache.oper_list().end(), Asset::PAI_OPER_TYPE_QIANGGANG);
	if (it == _oper_cache.oper_list().end()) return false;
	return true;
}

bool Game::CheckQiangGang(const Asset::PaiElement& pai, int64_t source_player_id)
{
	bool has_hu = false;

	int32_t next_player_index = (_curr_player_index + 1) % MAX_PLAYER_COUNT;

	for (int32_t i = next_player_index; i < MAX_PLAYER_COUNT - 1 + next_player_index; ++i)
	{
		auto cur_index = i % MAX_PLAYER_COUNT;

		auto player = GetPlayerByOrder(cur_index);
		if (!player || !player->CheckHuPai(pai, false, false)) continue;

		if (player->GetID() == source_player_id) continue;

		Asset::PaiOperationCache pai_operation;
		pai_operation.set_player_id(player->GetID());
		pai_operation.set_source_player_id(source_player_id);
		pai_operation.mutable_pai()->CopyFrom(pai);
		pai_operation.mutable_oper_list()->Add(Asset::PAI_OPER_TYPE_QIANGGANG);

		_oper_list.push_back(pai_operation);

		has_hu = true;
	}
	
	return has_hu;
}

void Game::OnOperateTimeOut()
{

}
	
void Game::SetPaiOperation(int64_t player_id, int64_t source_player_id, Asset::PaiElement pai, Asset::PAI_OPER_TYPE oper_type)
{
	_oper_cache.set_player_id(player_id); 
	_oper_cache.set_source_player_id(source_player_id); 
	_oper_cache.mutable_pai()->CopyFrom(pai);
	_oper_cache.mutable_oper_list()->Add(oper_type);
}
	
void Game::SetZiMoCache(std::shared_ptr<Player> player, Asset::PaiElement pai)
{
	if (!player) return;

	_oper_cache.set_player_id(player->GetID()); 
	_oper_cache.set_source_player_id(player->GetID()); 
	_oper_cache.mutable_pai()->CopyFrom(pai);
	_oper_cache.mutable_oper_list()->Add(Asset::PAI_OPER_TYPE_HUPAI);

	DEBUG("房间:{} 局数:{} 玩家:{}自摸设置数据:{}", _room_id, _game_id, player->GetID(), _oper_cache.ShortDebugString());
}

std::vector<int32_t> Game::TailPai(size_t card_count)
{
	std::vector<int32_t> tail_cards;
	std::vector<int32_t> cards(_cards.begin(), _cards.end());

	for (size_t i = 0; i < cards.size(); ++i)
	{
		if (_random_result_list.find(i + 1) == _random_result_list.end())  //不是宝牌缓存索引
		{
			_random_result_list.insert(i + 1);

			tail_cards.push_back(cards[cards.size() - 1 - i]);
			if (tail_cards.size() >= card_count) break;
		}
	}
	
	return tail_cards;
}
	
bool Game::CheckLiuJu()
{
	if (!_room) return false;

	if (GetRemainCount() > g_const->liuju_count() + 4) return false;
				
	_liuju = true;

	//
	//1.流局分张
	//
	Asset::LiuJu message;
	auto next_player_index = (_curr_player_index + 1) % MAX_PLAYER_COUNT;

	for (int32_t i = next_player_index; i < MAX_PLAYER_COUNT + next_player_index; ++i)
	{
		auto cur_index = i % MAX_PLAYER_COUNT;

		auto player = GetPlayerByOrder(cur_index);
		if (!player) return false;
		
		auto cards = FaPai(1); 
		const Asset::PaiElement card = GameInstance.GetCard(cards[0]); //玩家待抓的牌
		
		player->OnFaPai(card); //放入玩家牌内
	
		Asset::PaiOperation pai_operate;
		pai_operate.set_oper_type(Asset::PAI_OPER_TYPE_LIUJU);
		pai_operate.set_position(player->GetPosition());
		pai_operate.mutable_pai()->CopyFrom(card);
		AddPlayerOperation(pai_operate); //牌局回放

		//
		//各个玩家分张
		//
		auto ju_element = message.mutable_elements()->Add();
		ju_element->mutable_pai()->CopyFrom(card); 
		ju_element->set_player_id(player->GetID()); 
		
		//
		//缓存所有操作
		//
		Asset::PaiOperationCache pai_operation;
		pai_operation.set_player_id(player->GetID());
		pai_operation.set_source_player_id(player->GetID());
		pai_operation.mutable_pai()->CopyFrom(card);

		if (player->CheckZiMo(false)/* || player->CheckHuPai(card)*/) 
		{
			pai_operation.mutable_oper_list()->Add(Asset::PAI_OPER_TYPE_HUPAI);
			_oper_list.push_back(pai_operation);
		}
		else if (player->CheckBaoHu(card))
		{
			pai_operation.mutable_oper_list()->Add(Asset::PAI_OPER_TYPE_HUPAI);
			_oper_list.push_back(pai_operation);
		}
	}
	
	BroadCast(message);

	return true;
}

void Game::CalculateGangScore(Asset::GameCalculate& game_calculate)
{
	for (int i = 0; i < MAX_PLAYER_COUNT; ++i)
	{
		auto player = _players[i];
		if (!player) return;

		auto minggang = player->GetMingGang();
		auto ming_count = player->GetMingGangCount(); 
		auto an_count = player->GetAnGangCount(); 
		auto xf_count = player->GetXuanFengCount(); 
		
		int32_t ming_score_each = GetMultiple(Asset::FAN_TYPE_MING_GANG);
		int32_t ming_score = ming_count * ming_score_each;

		int32_t an_score = an_count * GetMultiple(Asset::FAN_TYPE_AN_GANG);
		int32_t xf_score = xf_count * GetMultiple(Asset::FAN_TYPE_XUAN_FENG_GANG);

		int32_t score = ming_score + an_score + xf_score; //玩家杠牌赢得其他单个玩家积分

		auto record = game_calculate.mutable_record()->mutable_list(i);
		record->set_score(record->score() + score * (MAX_PLAYER_COUNT - 1)); //增加杠牌玩家总杠积分

		//
		//1.杠牌玩家赢得积分列表
		//
		if (ming_count)
		{
			auto detail = record->mutable_details()->Add();
			detail->set_fan_type(Asset::FAN_TYPE_MING_GANG);
			detail->set_score(ming_score * (MAX_PLAYER_COUNT - 1));
		}

		if (an_count)
		{
			auto detail = record->mutable_details()->Add();
			detail->set_fan_type(Asset::FAN_TYPE_AN_GANG);
			detail->set_score(an_score * (MAX_PLAYER_COUNT - 1));
		}
		
		if (xf_count)
		{
			auto detail = record->mutable_details()->Add();
			detail->set_fan_type(Asset::FAN_TYPE_XUAN_FENG_GANG);
			detail->set_score(xf_score * (MAX_PLAYER_COUNT - 1));
		}
		//
		//2.其他玩家所输积分
		//
		for (int index = 0; index < MAX_PLAYER_COUNT; ++index)
		{
			if (index == i) continue;

			auto record = game_calculate.mutable_record()->mutable_list(index);
			auto player_id = record->player_id();

			record->set_score(record->score() - score); //扣除杠分

			//非杠牌玩家所输积分列表

			if (ming_count) //明杠，根据点杠玩家不同
			{
				auto ming_real = 0; //明杠分数

				for (const auto& gang : minggang)
				{
					DEBUG("玩家:{} 当前玩家:{} 点杠玩家:{} 杠牌:{}", player->GetID(), player_id, gang.source_player_id(), gang.ShortDebugString());


					if (_room->IsJianPing())
					{
						if (player_id == gang.source_player_id()) //点杠：建平玩法
						{
							auto detail = record->mutable_details()->Add();
							detail->set_fan_type(Asset::FAN_TYPE_MING_GANG);
							detail->set_score(detail->score() - ming_score_each * (MAX_PLAYER_COUNT - 1)); //点杠的那个人出3分

							ming_real += ming_score_each * (MAX_PLAYER_COUNT - 1);
						}
						else if (player->GetID() == gang.source_player_id()) //玩家过杠：每个人单独计分
						{
							auto detail = record->mutable_details()->Add();
							detail->set_fan_type(Asset::FAN_TYPE_MING_GANG);
							detail->set_score(detail->score() - ming_score_each); 

							ming_real += ming_score_each;
						}
					}
					else 
					{ 
						auto detail = record->mutable_details()->Add();
						detail->set_fan_type(Asset::FAN_TYPE_MING_GANG);
						detail->set_score(detail->score() - ming_score_each); 

						ming_real += ming_score_each;
					}
				}
			
				record->set_score(record->score() + ming_score - ming_real); //扣除杠分
					
				DEBUG("玩家:{} 当前玩家:{} 付分:{} 分数:{}", player->GetID(), player_id, ming_real, record->ShortDebugString());
			}

			if (an_count)
			{
				auto detail = record->mutable_details()->Add();
				detail->set_fan_type(Asset::FAN_TYPE_AN_GANG);
				detail->set_score(-an_score);
			}

			if (xf_count)
			{
				auto detail = record->mutable_details()->Add();
				detail->set_fan_type(Asset::FAN_TYPE_XUAN_FENG_GANG);
				detail->set_score(-xf_score);
			}
		} 
	} 
}
	
void Game::OnLiuJu()
{
	if (!_room) return;
		
	//
	//推到牌
	//
	PaiPushDown();
	
	//
	//记录本次杠牌积分
	//
	Asset::GameCalculate game_calculate;
	game_calculate.set_calculte_type(Asset::CALCULATE_TYPE_LIUJU);
	game_calculate.mutable_baopai()->CopyFrom(_baopai);

	for (auto player : _players)
	{
		if (!player) continue;
					   
		auto record = game_calculate.mutable_record()->mutable_list()->Add();
		record->set_player_id(player->GetID());
		record->set_nickname(player->GetNickName());
		record->set_headimgurl(player->GetHeadImag()); //理论上这种不用存盘，读取发给CLIENT的时候重新获取
	}

	if (!_room->HasHuangZhuang()) CalculateGangScore(game_calculate); //荒庄杠：流局杠分依然算
	
	BroadCast(game_calculate);
	
	_room->AddGameRecord(game_calculate.record()); //本局记录
	
	OnGameOver(0); 

	LOG(INFO, "房间:{}第:{}/{}局结束，胡牌玩家:{} 点炮玩家:{}, 流局结算:{}", _room_id, _game_id, _room->GetOpenRands(), 0, 0, game_calculate.ShortDebugString());
}

std::vector<int32_t> Game::FaPai(size_t card_count)
{
	std::vector<int32_t> cards;

	if (_cards.size() < card_count) return cards;

	for (size_t i = 0; i < card_count; ++i)
	{
		int32_t value = _cards.front();	
		cards.push_back(value);
		_cards.pop_front();
	}
	
	return cards;
}
	
void Game::FaPaiAndCommonCheck()
{
}
	
std::shared_ptr<Player> Game::GetNextPlayer(int64_t player_id)
{
	if (!_room) return nullptr;

	int32_t order = GetPlayerOrder(player_id);
	if (order == -1) return nullptr;

	return GetPlayerByOrder((order + 1) % MAX_PLAYER_COUNT);
}

int32_t Game::GetPlayerOrder(int32_t player_id)
{
	if (!_room) return -1;

	return _room->GetPlayerOrder(player_id);
}

std::shared_ptr<Player> Game::GetPlayerByOrder(int32_t player_index)
{
	if (!_room) return nullptr;

	if (player_index < 0 || player_index >= MAX_PLAYER_COUNT) return nullptr;

	return _players[player_index];
}

std::shared_ptr<Player> Game::GetPlayer(int64_t player_id)
{
	for (auto player : _players)
	{
		if (!player) continue;

		if (player->GetID() == player_id) return player;
	}
	
	return nullptr;
}

bool Game::IsBanker(int64_t player_id) 
{ 
	if (!_room) return false;
	return _room->IsBanker(player_id); 
}

Asset::PaiElement Game::GetBaoPai(int32_t tail_index)
{
	if (_random_result_list.find(tail_index) != _random_result_list.end()) tail_index += 6; //修正宝牌索引
		
	_random_result_list.insert(tail_index); //范围:1~6

	std::vector<int32_t> list(_cards.begin(), _cards.end());

	auto card_index = list.size() - tail_index; 

	auto baopai = GameInstance.GetCard(list[card_index]);
		
	Asset::PaiOperation pai_operate;
	pai_operate.set_oper_type(Asset::PAI_OPER_TYPE_BAOPAI);
	pai_operate.mutable_pai()->CopyFrom(baopai);
	AddPlayerOperation(pai_operate); //牌局回放

	DEBUG("房间:{} 局数:{} 生成宝牌:{}", _room_id, _game_id, baopai.ShortDebugString());

	return baopai;
}

int32_t Game::GetRemainBaopai()
{
	auto count = std::count_if(_cards_pool.begin(), _cards_pool.end(), [&](const Asset::PaiElement& pai){
			return _baopai.card_type() == pai.card_type() && _baopai.card_value() == pai.card_value();
			});
	return 3 - count; //墙上一张
}

//
//刷新宝牌，则通知全部听牌玩家
//
void Game::OnRefreshBaopai(int64_t player_id, int32_t random_result)
{
	if (!_room) return;

	Asset::RandomSaizi proto;
	proto.set_reason_type(Asset::RandomSaizi_REASON_TYPE_REASON_TYPE_TINGPAI);
	proto.mutable_random_result()->Add(random_result);
	proto.set_has_rand_saizi(true);

	for (auto player : _players)
	{
		if (!player) continue;

		if (player->IsTingPai())
		{
			player->ResetLookAtBaopai(); //玩家查看宝牌
			proto.mutable_pai()->CopyFrom(_baopai);
		}
		else
		{
			proto.mutable_pai()->Clear();
		}

		if (_room->HasAnbao()) proto.mutable_pai()->Clear(); //暗宝不同步宝牌
		player->SendProtocol(proto); 
	}
}
	
void Game::OnPlayerLookAtBaopai(int64_t player_id, Asset::RandomSaizi proto)
{
	proto.mutable_pai()->Clear();
	BroadCast(proto, player_id);
}
	
void Game::OnTingPai(std::shared_ptr<Player> player)
{
	if (!player) return;
	
	do {
		_random_result = CommonUtil::Random(1, 6);
		_baopai = GetBaoPai(_random_result);
	} while(GetRemainBaopai() <= 0); //直到产生还有剩余的宝牌
}
	
int32_t Game::GetMultiple(int32_t fan_type)
{
	if (!_room) return 0;
	
	return _room->GetMultiple(fan_type);
}

//
//游戏通用管理类
//
bool GameManager::Load()
{
	std::unordered_set<pb::Message*> messages = AssetInstance.GetMessagesByType(Asset::ASSET_TYPE_MJ_CARD);

	for (auto message : messages)
	{
		Asset::MJCard* asset_card = dynamic_cast<Asset::MJCard*>(message); 
		if (!asset_card) return false;
		
		for (int k = 0; k < asset_card->group_count(); ++k) //4组，麻将每张牌有4张
		{
			int32_t cards_count = std::min(asset_card->cards_count(), asset_card->cards_size());

			for (int i = 0; i < cards_count; ++i)
			{
				Asset::PaiElement card;
				card.set_card_type(asset_card->card_type());
				card.set_card_value(asset_card->cards(i).value());

				if (k == 0) _pais.push_back(card); //每张牌存一个
				_cards.emplace(_cards.size() + 1, card); //从1开始的索引
			}
		}
	}

	//if (_cards.size() != CARDS_COUNT) return false;
	return true;
}

void GameManager::OnCreateGame(std::shared_ptr<Game> game)
{
	_games.push_back(game);
}

}
