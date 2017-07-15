#pragma once

#include <map>
#include <mutex>
#include <cmath>
#include <memory>
#include <unordered_map>
#include <functional>

#include "P_Header.h"
#include "Item.h"
#include "Asset.h"
#include "MessageDispatcher.h"
#include "World.h"

namespace Adoter
{
namespace pb = google::protobuf;

class Room;
class Game;

struct Card_t {

	int32_t _type; //类型
	int32_t _value; //值

public:
	bool operator == (const Card_t& card)
	{
		return _type == card._type && _value == card._value;
	}

	Card_t operator + (int32_t value)
	{
		return Card_t(_type, _value + value);
	}

	Card_t(int32_t type, int32_t value)
	{
		_type = type;
		_value = value;
	}
};

extern const Asset::CommonConst* g_const;
//extern std::shared_ptr<CenterSession> g_center_session;

class Player : public std::enable_shared_from_this<Player>
{
	typedef std::function<int32_t(pb::Message*)> CallBack;
	std::unordered_map<int32_t, CallBack>  _callbacks;	//每个协议的回调函数，不要传入引用
private:
	int64_t _player_id = 0; //玩家ID
	Asset::Player _stuff; //玩家数据，存盘数据
	Asset::PlayerProp _player_prop; //玩家临时状态，不进行存盘

	int64_t _heart_count = 0; //心跳次数
	int32_t _hi_time = 0; 
	bool _dirty = false; //脏数据
	CallBack _method; //协议处理回调函数
public:
	Player();
	Player(int64_t player_id);
	//Player(int64_t player_id, std::shared_ptr<WorldSession> session);
	
	//const std::shared_ptr<WorldSession> GetSession() { return _session;	}
	//bool Connected() { if (!_session) return false; return _session->IsConnect(); }

	int32_t DefaultMethod(pb::Message*); //协议处理默认调用函数
	
	void AddHandler(Asset::META_TYPE message_type, CallBack callback)
	{
		if (_callbacks.find(message_type) != _callbacks.end()) return;

		_callbacks.emplace(message_type, callback);
	}

	CallBack& GetMethod(int32_t message_type)
	{
		auto it = _callbacks.find(message_type);
		if (it == _callbacks.end()) return _method;
		return it->second;
	}

	Asset::Player& Get() { return _stuff; } //获取玩家数据

	//获取基础属性
	const Asset::CommonProp& CommonProp() { return _stuff.common_prop(); }
	const Asset::CommonProp& GetCommonProp() { return _stuff.common_prop(); }
	Asset::CommonProp* MutableCommonProp() { return _stuff.mutable_common_prop(); }

	//微信数据
	const Asset::WechatUnion GetWechat();

	int32_t GetLocalServer() { return _stuff.server_id(); } //玩家当前所在服务器
	void SetLocalServer(int32_t server_id) { return _stuff.set_server_id(server_id); }

	virtual int64_t GetID() { return _stuff.common_prop().player_id(); } //获取ID
	virtual void SetID(int64_t player_id) { 
		_player_id = player_id; //缓存
		_stuff.mutable_common_prop()->set_player_id(player_id); 
	} 
	//获取名字
	virtual std::string GetName() { return _stuff.common_prop().name(); }
	virtual void SetName(std::string name) { _stuff.mutable_common_prop()->set_name(name); } 
	//获取级别
	virtual int32_t GetLevel() { return _stuff.common_prop().level(); }
	//获取性别
	virtual int32_t GetGender() { return _stuff.common_prop().gender(); }
	//消息处理
	virtual bool HandleMessage(const Asset::MsgItem& item); 
	virtual void SendMessage(Asset::MsgItem& item);
	virtual void BroadCastCommonProp(Asset::MSG_TYPE type); //向房间里的玩家发送公共数据       
	//协议处理(Protocol Buffer)
	virtual bool HandleProtocol(int32_t type_t, pb::Message* message);
	virtual void SendProtocol(const pb::Message& message);
	virtual void SendProtocol(const pb::Message* message);
	virtual void Send2Roomers(pb::Message& message, int64_t exclude_player_id = 0); //向房间里玩家发送协议数据，发送到Client
	virtual void Send2Roomers(pb::Message* message, int64_t exclude_player_id = 0); //向房间里玩家发送协议数据，发送到Client
	virtual void BroadCast(Asset::MsgItem& item);
	//virtual void OnCreatePlayer(int64_t player_id);
	//进入游戏
	//virtual int32_t CmdEnterGame(pb::Message* message);
	virtual int32_t OnEnterGame();
	//创建房间
	virtual int32_t CmdCreateRoom(pb::Message* message);
	virtual void OnCreateRoom(Asset::CreateRoom* create_room);
	//进入房间
	virtual int32_t CmdEnterRoom(pb::Message* message);
	virtual bool OnEnterRoom(int64_t room_id = 0);
	//玩家登录
	virtual int32_t OnLogin();
	//玩家登出
	virtual int32_t Logout(pb::Message* message);
	virtual int32_t OnLogout();
	//离开房间
	virtual int32_t CmdLeaveRoom(pb::Message* message);
	virtual void OnLeaveRoom();
	//加载数据	
	virtual int32_t Load();
	//保存数据
	virtual int32_t Save();
	//是否脏数据
	virtual bool IsDirty() { return _dirty; }
	virtual void SetDirty(bool dirty = true) { _dirty = dirty; }
	//同步玩家数据
	virtual void SendPlayer();
	//玩家心跳周期为10MS，如果该函数返回FALSE则表示掉线
	virtual bool Update();
	//购买商品
	virtual bool CmdBuySomething(pb::Message* message);
	//是否在线
	bool IsOnline() { return _stuff.login_time() != 0; }
	//签到
	virtual int32_t CmdSign(pb::Message* message);
	//获取玩家基础属性
	virtual int32_t CmdGetCommonProperty(pb::Message* message);
	void SyncCommonProperty(Asset::SyncCommonProperty_SYNC_REASON_TYPE reason = Asset::SyncCommonProperty_SYNC_REASON_TYPE_SYNC_REASON_TYPE_SELF);
	//离线状态实时监测
	virtual int32_t CmdSayHi(pb::Message* message);
	void SayHi();
	//游戏设置
	virtual int32_t CmdGameSetting(pb::Message* message);

	//踢下线
	virtual int32_t OnKickOut(pb::Message* message);
public:
	//获取所有包裹
	const Asset::Inventory& GetInventory() { return _stuff.inventory();	}

	//获取指定包裹
	const Asset::Inventory_Element& GetInventory(Asset::INVENTORY_TYPE type) { return _stuff.inventory().inventory(type); }	
	Asset::Inventory_Element* GetMutableInventory(Asset::INVENTORY_TYPE type) { return _stuff.mutable_inventory()->mutable_inventory(type);	}	
	
	//获取物品
	bool GainItem(Item* item, int32_t count = 1);
	bool GainItem(int64_t global_item_id, int32_t count = 1);
	bool PushBackItem(Asset::INVENTORY_TYPE inventory_type, Item* item); //存放物品

	//通用错误码提示
	void AlertMessage(Asset::ERROR_CODE error_code, Asset::ERROR_TYPE error_type = Asset::ERROR_TYPE_NORMAL, Asset::ERROR_SHOW_TYPE error_show_type = Asset::ERROR_SHOW_TYPE_CHAT);

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	//
	// 欢乐豆相关
	//
	int64_t ConsumeHuanledou(Asset::HUANLEDOU_CHANGED_TYPE changed_type, int64_t count); //消费欢乐豆(返回实际消耗的欢乐豆数)
	int64_t GainHuanledou(Asset::HUANLEDOU_CHANGED_TYPE changed_type, int64_t count); //增加欢乐豆
	bool CheckHuanledou(int64_t count); //欢乐豆是否足够
	int64_t GetHuanledou(); //获取欢乐豆数量

	//
	// 钻石相关
	//
	int64_t ConsumeDiamond(Asset::DIAMOND_CHANGED_TYPE changed_type, int64_t count); //消费钻石(返回实际消耗的钻石数)
	int64_t GainDiamond(Asset::DIAMOND_CHANGED_TYPE changed_type, int64_t count); //增加钻石
	bool CheckDiamond(int64_t count); //钻石是否足够
	int64_t GetDiamond(); //获取钻石数量
	
	//
	// 房卡相关
	//
	int64_t ConsumeRoomCard(Asset::ROOM_CARD_CHANGED_TYPE changed_type, int64_t count); //消费房卡(返回实际消耗的房卡数)
	int64_t GainRoomCard(Asset::ROOM_CARD_CHANGED_TYPE changed_type, int64_t count); //增加房卡
	bool CheckRoomCard(int64_t count); //房卡是否足够
	int64_t GetRoomCard(); //获取房卡数量
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

	//通用限制
	const Asset::PlayerCommonLimit& GetCommonLimit() { return _stuff.common_limit(); }
	Asset::PlayerCommonLimit* GetMutableCommonLimit() {	return _stuff.mutable_common_limit(); }
	bool AddCommonLimit(int64_t global_id);
	bool IsCommonLimit(int64_t global_id);
	bool CommonLimitUpdate();
	void SyncCommonLimit();
	//通用奖励
	Asset::ERROR_CODE DeliverReward(int64_t global_id);
	void SyncCommonReward(int64_t common_reward_id);
///////游戏逻辑定义
private:
	std::mutex _card_lock;
	std::shared_ptr<Room> _room = nullptr; //实体所在房间
	std::shared_ptr<Game> _game = nullptr; //当前游戏

	//玩家牌数据
	std::map<int32_t/*麻将牌类型*/, std::vector<int32_t>/*牌值*/> _cards_inhand; //玩家手里的牌
	std::map<int32_t/*麻将牌类型*/, std::vector<int32_t>/*牌值*/> _cards_outhand; //玩家墙外牌
	std::vector<Asset::PaiElement> _minggang; //明杠
	std::vector<Asset::PaiElement> _angang; //暗杠
	int32_t _jiangang = 0; //旋风杠，本质是明杠
	int32_t _fenggang = 0; //旋风杠，本质是暗杠

	int32_t _oper_count = 0; //牌操作次数
	int32_t _oper_count_tingpai = 0; //听牌后操作次数
	bool _has_ting = false; //听牌
	bool _tuoguan_server = false; //服务器托管
	//bool _tuoguan_client = false; //Client托管

	std::vector<Asset::PAI_OPER_TYPE> _xf_gang; //旋风杠所有操作
	std::vector<std::tuple<bool, bool, bool>> _hu_result;
	Asset::PAI_OPER_TYPE _oper_type; //玩家上次操作类型，主要用于处理杠上开的番次

	void DebugCommand();
public:
	//玩家操作
	virtual int32_t CmdGameOperate(pb::Message* message); //游戏操作
	virtual int32_t CmdPaiOperate(pb::Message* message); //牌操作
	virtual int32_t CmdGetReward(pb::Message* message); //领取奖励
	virtual int32_t CmdLoadScene(pb::Message* message); //加载场景
	virtual int32_t CmdLuckyPlate(pb::Message* message); //幸运转盘
	virtual int32_t CmdSaizi(pb::Message* message); //打股子
	void OnEnterScene();
	//获取房间
	virtual std::shared_ptr<Room> GetRoom() { return _room; }	//获取当前房间
	virtual void SetRoomID(int64_t room_id) { _player_prop.set_room_id(room_id); }	
	virtual int32_t GetRoomID() { return _player_prop.room_id(); }
	virtual bool HasRoom() { return _room != nullptr; }
	virtual void SetRoom(std::shared_ptr<Room> room) { _room = room; }

	void SetGame(std::shared_ptr<Game> game) { _game = game; }
	bool IsInGame() { return _game != nullptr; }

	virtual int32_t OnFaPai(std::vector<int32_t>& cards); //发牌

	std::vector<Asset::PAI_OPER_TYPE> CheckPai(const Asset::PaiElement& pai, int64_t from_player_id);

	bool CheckBaoHu(const Asset::PaiElement& pai);

	bool CanHuPai(std::vector<Card_t>& cards, bool use_pair = false);

	bool CheckHuPai(); //胡牌检查:玩家手里现有牌检查
	bool CheckHuPai(const Asset::PaiElement& pai); //胡牌
	bool CheckHuPai(std::vector<Asset::FAN_TYPE>& fan_list); //胡牌且算番数
	bool CheckHuPai(const Asset::PaiElement& pai, std::vector<Asset::FAN_TYPE>& fan_list); //胡牌且算番数
	bool CheckHuPai(const std::map<int32_t, std::vector<int32_t>>& cards_inhand, //玩家手里的牌
			const std::map<int32_t, std::vector<int32_t>>& cards_outhand, //玩家墙外牌
			const std::vector<Asset::PaiElement>& minggang, //明杠
			const std::vector<Asset::PaiElement>& angang, //暗杠
			int32_t jiangang, //旋风杠，本质是明杠
			int32_t fenggang, //旋风杠，本质是暗杠
			const Asset::PaiElement& pai); //胡牌

	bool CheckGangPai(const Asset::PaiElement& pai, int64_t from_player_id); //是否可以杠牌
	//有玩家一直不杠牌, 每次都要提示, 比如玩家碰了7条,但是手里有7-8-9条,而选择暂时不杠
	bool CheckAllGangPai(::google::protobuf::RepeatedField<Asset::PaiOperationAlert_AlertElement>& gang_list); 

	void OnGangPai(const Asset::PaiElement& pai, int64_t from_player_id); //杠牌
	
	bool CheckFengGangPai(std::map<int32_t/*麻将牌类型*/, std::vector<int32_t>/*牌值*/>& cards); //是否有旋风杠
	bool CheckJianGangPai(std::map<int32_t/*麻将牌类型*/, std::vector<int32_t>/*牌值*/>& cards); //是否有箭杠
	void OnGangFengPai(); //旋风杠-风杠
	void OnGangJianPai(); //旋风杠-箭杠

	std::vector<Asset::PAI_OPER_TYPE> GetXuanFeng() { return _xf_gang; } //旋风杠获取

	bool CheckFengGangPai(); //是否可以旋风杠-风杠 
	bool CheckJianGangPai(); //是否可以旋风杠-箭杠

	int32_t CheckXuanFeng(); //检查旋风杠
	
	bool CanTingPai(const Asset::PaiElement& pai);
	bool CheckTingPai(std::vector<Asset::PaiElement>& pais/*应该打出的牌数据*/); //是否可以听牌：能不能听牌，主要是看是否给牌可以胡

	bool CheckPengPai(const Asset::PaiElement& pai); //是否可以碰牌
	void OnPengPai(const Asset::PaiElement& pai); //碰牌

	bool CheckChiPai(const Asset::PaiElement& pai); //是否可以吃牌
	void OnChiPai(const Asset::PaiElement& pai, pb::Message* message); //吃牌

	bool IsKaimen() { return _cards_outhand.size() != 0 || _minggang.size() != 0; } //是否开门
	bool IsBimen() { return _cards_outhand.size() == 0 && _minggang.size() == 0; } //是否闭门

	bool CheckMingPiao(const Asset::PAI_OPER_TYPE& oper_type);

	bool IsTingPai() { return _has_ting; } //是否听牌
	bool HasTingPai() { return _has_ting; } //是否听牌

	void OnTingPai(); //听牌响应
	void OnGameStart();

	bool IsMingPiao(); //是否明飘
	bool IsSiGuiYi(); //是否四归一 

	int32_t GetMingGangCount() { return _minggang.size(); } //明杠数量
	int32_t GetAnGangCount() { return _angang.size(); } //暗杠数量
	int32_t GetXuanFengCount() { return _jiangang + _fenggang; } //旋风杠数量

	void PreCheckOnFaPai(); //发牌前置检查
	bool IsReady() { return _player_prop.game_oper_state() == Asset::GAME_OPER_TYPE_START; } //是否已经在准备状态 
	Asset::GAME_OPER_TYPE GetOperState() { return _player_prop.game_oper_state(); }
	void SetOperState(Asset::GAME_OPER_TYPE oper_type) { return _player_prop.set_game_oper_state(oper_type); }

	//获取//设置玩家座次
	Asset::POSITION_TYPE GetPosition() { return _player_prop.position(); }
	void SetPosition(Asset::POSITION_TYPE position) { _player_prop.set_position(position); }
	
	void PrintPai(); //打印牌玩家当前牌
	void SynchronizePai(); //同步玩家牌给Client

	const std::map<int32_t, std::vector<int32_t>>& GetCardsInhand() { return _cards_inhand; }

	void ClearCards(); //删除玩家牌(包括手里牌、墙外牌)
	void OnGameOver(); //游戏结束

	//是否//设置服务器托管状态
	bool HasTuoGuan() { return _tuoguan_server; }
	void SetTuoGuan() { _tuoguan_server = true; }

	int32_t GetCardCount();	//获取当前玩家手中牌数
	bool IsGangOperation(); //上次牌是否杠操作

	bool AddGameRecord(const Asset::GameRecord& record);
};

/////////////////////////////////////////////////////
//玩家通用管理类
/////////////////////////////////////////////////////
class PlayerManager : public std::enable_shared_from_this<PlayerManager>
{
private:
	std::mutex _mutex;
	std::unordered_map<int64_t, std::shared_ptr<Player>> _players; //实体为智能指针，不要传入引用
public:
	static PlayerManager& Instance()
	{
		static PlayerManager _instance;
		return _instance;
	}

	void Update(int32_t diff);

	void Remove(int64_t player_id);
	void Remove(std::shared_ptr<Player> player);

	void Emplace(int64_t player_id, std::shared_ptr<Player> player);

	bool Has(int64_t player_id);

	std::shared_ptr<Player> GetPlayer(int64_t player_id);
	std::shared_ptr<Player> Get(int64_t player_id);
	
	virtual void BroadCast(const pb::Message& message);
};

#define PlayerInstance PlayerManager::Instance()

}
