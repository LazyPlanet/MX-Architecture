usage()
{ 
  echo "Usage: $0 {start|stop|restart|status}" 
} 

suffix=""

status() 
{
	check_process GameServer
	check_process GmtServer
}

check_process()
{
	SELFID=`id -u`
	PIDS=`pgrep -u $SELFID -f "./$1$suffix.*$1.conf"`
	if [ ! -z "$PIDS" ]; then
		echo Service $1 is running, Pid is $PIDS
	else
		echo Service $1 is stopped.
	fi
}

exists()
{
	SELFID=`id -u`
	PIDS=`pgrep -u $SELFID -f "./$1$suffix.*$1.conf"`
	if [ ! -z "$PIDS" ]; then
		echo Service $1 is running, Pid is $PIDS
		return 1
	fi
	return 0
}

kill_process()
{
	NAME=$1
	SIG=$2
	SELFID=`id -u`
	PIDS=`pgrep -u $SELFID -f "./$NAME$suffix.*$NAME.conf"`
	if [ -n "$PIDS" ]; then
		for PID in $PIDS
		do
			kill -$SIG $PID
			echo Service $NAME pid $PID is killed.
		done
	else
		echo Service $NAME is not running.
	fi
}

start()
{
	mkdir -p logs
	
	ulimit -c unlimited
	ulimit -n 4096
    
	export LD_LIBRARY_PATH=.:../../lib:/home/game/MX/ThirdParty/boost_1_63_0/stage/lib

	exists GameServer
	if [ $? -eq 0 ]; then
		echo 'Start GameServer Service.'
		nohup setsid ./GameServer GameServer.conf &>./gameserver.log  &
		#cd ..
	fi
	
	exists GmtServer
	if [ $? -eq 0 ]; then
		echo 'Start GmtServer Service.'
		nohup setsid ./GmtServer GmtServer.conf &>./gmtserver.log  &
		#cd ..
	fi
}

stop()
{
	kill_process GameServer KILL
	kill_process GmtServer KILL
} 

wait()
{
	for (( i=0; i<5; i++))
	do
		echo -n "."
		sleep 0.2
	done
}

restart()
{
    stop $1
	wait
    
	start $1
	
	wait
	echo 'Game servers has accomplished success.'
} 

case $1 in 
start) 
        start $2
        ;; 
stop)
        stop $2
        ;; 
restart)
        restart $2
        ;; 
status)
        status 
        ;; 
*) 
        usage 
        ;;
esac 
