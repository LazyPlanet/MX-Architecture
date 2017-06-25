/*
 * C++版本：C++11
 *
 * 说明：倾向于用标准类库中接口
 *
 */

#include <thread>
#include <vector>
#include <memory>
#include <functional>

#include <boost/asio.hpp>
#include <boost/bind.hpp>

#include "Timer.h"
#include "World.h"
#include "MXLog.h"
#include "Config.h"
#include "ClientSession.h"
#include "CenterSession.h"
#include "WorldSession.h"

const int const_world_sleep = 50;

/*
 * 游戏逻辑处理接口
 *
 * */

using namespace Adoter;

boost::asio::io_service _io_service;
std::shared_ptr<boost::asio::io_service::work> _io_service_work;

void SignalHandler(const boost::system::error_code& error, int)
{    
	//if (!error) World::StopNow(SHUTDOWN_EXIT_CODE);
}

void WorldUpdateLoop()
{
	int32_t curr_time = 0, prev_sleep_time = 0;
	
	int32_t prev_time = CommonTimerInstance.GetStartTime();

	while (!WorldInstance.IsStopped())
	{
		curr_time = CommonTimerInstance.GetStartTime();
		
		int32_t diff = CommonTimerInstance.GetTimeDiff(prev_time, curr_time);
		
		WorldInstance.Update(diff);        
		
		prev_time = curr_time;
		
		if (diff <= const_world_sleep + prev_sleep_time) //50MS
		{            
			prev_sleep_time = const_world_sleep + prev_sleep_time - diff;            
			std::this_thread::sleep_for(std::chrono::milliseconds(prev_sleep_time));        
		}        
		else    
		{	
			prev_sleep_time = 0;
		}
	}
}

void ShutdownThreadPool(std::vector<std::shared_ptr<std::thread>>& threads)
{    
	_io_service.stop();    
	
	for (auto& thread : threads) thread->join();    
}

/*
 *
 * 函数入口
 *
 * */
int main(int argc, const char* argv[])
{
		/*	
		Asset::Item_Potion* message = new Asset::Item_Potion();
		message->mutable_item_common_prop()->set_quality(5);
		Item* item = new Item(message);	
		bool re = item->CanUse();
		Item* item_potion = new Item_Potion(message);	
		*/
	if (argc != 2) return 2; //参数不对

	try 
	{
		std::srand(std::time(0)); //random_shuffle不是真随机：http://stackoverflow.com/questions/13459953/random-shuffle-not-really-random

		//系统配置读取
		if (!ConfigInstance.LoadInitial(argv[1]))
		{
			std::cout << "Load file error, please check the file, name:" << argv[1] << std::endl;; //控制台的日志可以直接用该函数
			return 3;
		}
	
		//日志系统配置
		MXLogInstance.Load();
	
/////////////////////////////////////////////////////游戏逻辑初始化

		//世界初始化，涵盖所有....
		if (!WorldInstance.Load()) return 1;

		//网络初始化
		_io_service_work = std::make_shared<boost::asio::io_service::work>(_io_service);

		int _thread_nums = 5;
		std::vector<std::shared_ptr<std::thread>> _threads;	
		for (int i = 0; i < _thread_nums; ++i)
		{
			std::shared_ptr<std::thread> pthread(new std::thread(boost::bind(&boost::asio::io_service::run, &_io_service)));	//IO <-> Multi Threads
			_threads.push_back(pthread);
		}

		//boost::asio::signal_set signals(_io_service, SIGINT, SIGTERM);
		//signals.async_wait(SignalHandler);
		//

		std::string server_ip = ConfigInstance.GetString("ServerIP", "0.0.0.0");
		if (server_ip.empty()) return 4;
		
		int32_t server_port = ConfigInstance.GetInt("ServerPort", 50000);
		if (server_port <= 0 || server_port > 0xffff) return 5;
		
		int32_t thread_count = ConfigInstance.GetInt("ThreadCount", 5);
		if (thread_count <= 0) return 6;

		WorldSessionInstance.StartNetwork(_io_service, server_ip, server_port, thread_count);

		//GMT服Client连接
		boost::asio::ip::tcp::endpoint gmt_endpoint(boost::asio::ip::address::from_string("0.0.0.0"), 50003);
		auto _gmt_client = std::make_shared<ClientSession>(_io_service, gmt_endpoint);
		_gmt_client->AsyncConnect();
		
		//中心器Client连接
		boost::asio::ip::tcp::endpoint center_endpoint(boost::asio::ip::address::from_string("0.0.0.0"), 50004);
		auto _center_client = std::make_shared<CenterSession>(_io_service, center_endpoint);
		_center_client->AsyncConnect();

		//世界循环
		WorldUpdateLoop();
	
		DEBUG("Service stop.");

		ShutdownThreadPool(_threads);
	}
	catch (const std::exception& e)
	{
		ERROR("Exception:{}", e.what());
	}
	
	DEBUG("Service stopped.");
	return 0;
}
