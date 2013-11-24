import sys
import socket
import json
import getopt
from threading import Thread

MASTER_HOST = 'localhost'
MASTER_PORT = 5557
ADMIN_VERSION = '0.1'

def Exit(msg):
    print( msg )
    print( "exiting..." )
    sys.exit( 1 )

class Connection():
    def __init__(self):
        print( "connecting to master %s:%d" % (MASTER_HOST, MASTER_PORT) )
        try:
            self.socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            self.socket.connect((MASTER_HOST, MASTER_PORT))
            self.rpc_id = 0
            print( "connected" )
        except Exception as e:
            Exit( "couldn't connect to master" )

    def MakeJsonRpc(self, msg):
        rpc = { "jsonrpc" : "2.0", "method" : msg["method"], "params" : msg["params"], "id" : str( self.rpc_id ) }
        self.rpc_id += 1
        return json.JSONEncoder().encode( rpc )

    def Send(self, msg):
        rpc = self.MakeJsonRpc( msg )
        try:
            self.socket.send( rpc.encode( "utf-8" ) )
        except Exception as e:
            Exit( "couldn't send command to master" )

    def Receive(self):
        try:
            data = self.socket.recv( 32 * 1024 )
            return data
        except Exception as e:
            pass

    def Close(self):
        try:
            self.socket.shutdown(socket.SHUT_RDWR)
            self.socket.close()
        except Exception as e:
            pass

class ResultGetter(Thread):
    def __init__( self, connection ):
        Thread.__init__(self)
        self.connection = connection
    
    def run(self):
        while True:
            msg = self.connection.Receive()
            if msg is None or len(msg) == 0:
                break

            self.OnResponse( msg )
            sys.stdout.write( '> ' )
            sys.stdout.flush()

    def OnResponse(self, msg):
        try:
            msg = msg.decode( "utf-8" )
            rpc = json.JSONDecoder().decode( msg )

            if 'result' in rpc:
                print( rpc['result'] )
            elif 'error' in rpc:
                print( rpc['error'] )
        except Exception as e:
            print( e )

class Command_Run():
    def Prepare(self, cmd):
        try:
            path = cmd.split()[1]
        except Exception as e:
            print( "no file path given" )
            raise e
        return {"method" : "run", "params" : {"file" : path} }

class Command_Stop():
    def Prepare(self, cmd):
        try:
            jobId = int( cmd.split()[1] )
        except Exception as e:
            print( "invalid jobId argument" )
            raise e
        return {"method" : "stop", "params" : {"job_id" : jobId} }

class Command_StopGroup():
    def Prepare(self, cmd):
        try:
            groupId = int( cmd.split()[1] )
        except Exception as e:
            print( "invalid groupId argument" )
            raise e
        return {"method" : "stop_group", "params" : {"group_id" : groupId} }

class Command_StopAll():
    def Prepare(self, cmd):
        return {"method" : "stop_all", "params" : []}

class Command_StopPrevious():
    def Prepare(self, cmd):
        return {"method" : "stop_prev", "params" : []}

class Command_AddHosts():
    def Prepare(self, cmd):
        try:
            hosts = cmd.split()[1:]
            if len( hosts ) % 2 > 0:
                raise Exception( "Odd number of args" )
        except Exception as e:
            print( "invalid <host, group> arguments" )
            raise e
        return {"method" : "add_hosts", "params" : {"hosts" : hosts} }

class Command_DeleteHosts():
    def Prepare(self, cmd):
        try:
            hosts = cmd.split()[1:]
        except Exception as e:
            print( "invalid host argument" )
            raise e
        return {"method" : "delete_hosts", "params" : {"hosts" : hosts} }

class Command_AddHostGroup():
    def Prepare(self, cmd):
        try:
            path = cmd.split()[1]
        except Exception as e:
            print( "no file path given" )
            raise e
        return {"method" : "add_group", "params" : {"file" : path} }

class Command_DeleteHostGroup():
    def Prepare(self, cmd):
        try:
            group = cmd.split()[1]
        except Exception as e:
            print( "invalid groupName argument" )
            raise e
        return {"method" : "delete_group", "params" : {"group" : group} }

class Command_Info():
    def Prepare(self, cmd):
        try:
            jobId = int( cmd.split()[1] )
        except Exception as e:
            print( "invalid jobId argument" )
            raise e
        return {"method" : "info", "params" : {"job_id" : jobId} }

class Command_Stat():
    def Prepare(self, cmd):
        return {"method" : "stat", "params" : []}

class Command_Test():
    def Prepare(self, cmd):
        msg = '{"method":"run","params":{"file":"/home/budnik/dev/prun/test/test.job"}}'
        return msg


class CommandDispatcher():
    _instance = None
    def __init__(self):
        self.map_ = {'run'     : Command_Run(),
                     'stop'    : Command_Stop(),
                     'stopg'   : Command_StopGroup(),
                     'stopall' : Command_StopAll(),
                     'stoprev' : Command_StopPrevious(),
                     'add'     : Command_AddHosts(),
                     'delete'  : Command_DeleteHosts(),
                     'addg'    : Command_AddHostGroup(),
                     'deleteg' : Command_DeleteHostGroup(),
                     'info'    : Command_Info(),
                     'stat'    : Command_Stat(),
                     'test'    : Command_Test()}

    @classmethod
    def Instance(cls):
        if cls._instance is None:
            cls._instance = CommandDispatcher()
        return cls._instance

    def Get(self, command):
        cmd = command.split( None, 1 )[0]
        if cmd not in self.map_:
            return None
        return self.map_[ cmd ]

class Master():
    def __init__(self, connection):
        self.connection = connection

    def DoCommand(self, cmd):
        dispatcher = CommandDispatcher.Instance()
        handler = dispatcher.Get( cmd )
        if handler is not None:
            try:
                msg = handler.Prepare( cmd )
                self.connection.Send( msg )
            except Exception as e:
                print( "error: couldn't execute command" )
                print( e )
        else:
            print( "unknown command: " + cmd )

def PrintHelp():
    print( "Commands:" )
    print( "  run /path/to/job/file          -- run job, which described in '.job' or '.meta' file" )
    print( "  stop <job_id>                  -- interrupt job execution" )
    print( "  stopg <group_id>               -- interrupt group of jobs execution" )
    print( "  stopall                        -- interrupt all job execution on all hosts" )
    print( "  stoprev                        -- interrupt all job execution from previous master sessions" )
    print( "  add [<hostname> <groupname>]*  -- add host(s) with given hostname and hosts group name" )
    print( "  delete <hostname>              -- delete host(s)" )
    print( "  addg /path/to/host/group/file  -- add group of hosts, which described in a file" )
    print( "  deleteg <groupname>            -- delete group of hosts" )
    print( "  info <job_id>                  -- show job execution statistics" )
    print( "  stat                           -- show master statistics" )
    print( "  repeat, r                      -- repeat last entered command" )
    print( "  exit, e                        -- quit program" )

def UserPrompt():
    print( "master admin v" + ADMIN_VERSION )
    print( "print `help` for more information" )

def ParseOpt( argv ):
    global MASTER_HOST
    try:
        opts, args = getopt.getopt( argv, '' )
        for arg in args:
            MASTER_HOST = arg
            break
    except getopt.GetoptError:
        print( 'usage: admin.py [host]' )
        sys.exit( 1 )

def Main(argv):
    ParseOpt( argv )
    UserPrompt()
    con = Connection()
    master = Master( con )
    resultGetter = ResultGetter( con )
    resultGetter.start()

    try:
        lastCmd = None
        while True:
            sys.stdout.write( '> ' )
            sys.stdout.flush()

            line = sys.stdin.readline().strip()
            if len(line) == 0:
                continue

            if line in ("exit", "e", "quit", "q"):
                break
            if line == "help":
                PrintHelp()
                continue
            if line in ("repeat", "r") and lastCmd is not None:
                line = lastCmd

            master.DoCommand( line )
            lastCmd = line
    except Exception as e:
        print( e )

    con.Close()
    resultGetter.join()

if __name__ == "__main__":
   Main(sys.argv[1:])
