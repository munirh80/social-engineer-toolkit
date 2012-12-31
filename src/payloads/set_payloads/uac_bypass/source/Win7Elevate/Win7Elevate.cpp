#include "stdafx.h"
#include "resource.h"
#include "Win7Elevate_Utils.h"
#include "Win7Elevate_Inject.h"

#include <iostream>
#include <sstream>
#include <algorithm>

#include ".\..\Redirector.h"
#include ".\..\CMMN.h"

//
//	By Pavlov P.S. (PavPS)
//

void PrintUsage()
{
	std::cout << "Incorrect input. Please find samples below. " << std::endl;
	std::cout << "Note, 'elevate stuff' will be executed in the elevated shell as 'cmd.exe stuff' " << std::endl;
	std::cout << "\televate /c <ANY COMMAND SEQUENCE THAT IS ALLOWED BY CMD.EXE SHELL>" << std::endl;
	std::cout << "\televate /c <command> [arg1] [arg2] .. [argn]" << std::endl;
	std::cout << "\televate --pid 1234 /c <command> [arg1] [arg2] .. [argn]" << std::endl;
	std::cout << "\televate /c c:\\path\\foo.exe [arg1] [arg2] .. [argn]" << std::endl;
	std::cout << "\televate --pid 1234 /c c:\\path\\foo.exe [arg1] [arg2] .. [argn]" << std::endl;
}

HANDLE PipeIn = NULL;
OVERLAPPED PipeInO;

HANDLE PipeOut = NULL;
OVERLAPPED PipeOutO;

HANDLE PipeErr = NULL;
OVERLAPPED PipeErrO;

//
//	Initializes named pipes that will be used for connection with TIOR
//
bool SetupNamedPipe()
{
	PipeInO.hEvent = CreateEvent( NULL, TRUE, FALSE, NULL );
	PipeIn = CreateNamedPipe( 
		STDIn_PIPE, 
		PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE | FILE_FLAG_WRITE_THROUGH | FILE_FLAG_OVERLAPPED, 
		PIPE_TYPE_BYTE | PIPE_WAIT,  
		PIPE_UNLIMITED_INSTANCES, 
		0, 0, 
		NMPWAIT_USE_DEFAULT_WAIT, 
		NULL );

	ConnectNamedPipe( PipeIn, &PipeInO );

	PipeOutO.hEvent = CreateEvent( NULL, TRUE, FALSE, NULL );
	PipeOut = CreateNamedPipe( 
		STDOut_PIPE, 
		PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE | FILE_FLAG_WRITE_THROUGH | FILE_FLAG_OVERLAPPED, 
		PIPE_TYPE_BYTE | PIPE_WAIT,  
		PIPE_UNLIMITED_INSTANCES, 
		0, 0, 
		NMPWAIT_USE_DEFAULT_WAIT, 
		NULL );

	ConnectNamedPipe( PipeOut, &PipeOutO );

	PipeErrO.hEvent = CreateEvent( NULL, TRUE, FALSE, NULL );
	PipeErr = CreateNamedPipe( 
		STDErr_PIPE, 
		PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE | FILE_FLAG_WRITE_THROUGH | FILE_FLAG_OVERLAPPED, 
		PIPE_TYPE_BYTE | PIPE_WAIT,  
		PIPE_UNLIMITED_INSTANCES, 
		0, 0, 
		NMPWAIT_USE_DEFAULT_WAIT, 
		NULL );

	ConnectNamedPipe( PipeErr, &PipeErrO );

	return true;
}

//
//	Initiates data pumping.
//
DWORD __stdcall Redirector()
{
	if ( !PipeIn )
		return -1;

	if ( PipeInO.hEvent )
		WaitForSingleObject( PipeInO.hEvent, -1000 );
	if ( PipeOutO.hEvent )
		WaitForSingleObject( PipeOutO.hEvent, -1000 );
	if ( PipeErrO.hEvent )
		WaitForSingleObject( PipeErrO.hEvent, -1000 );

	TRedirectorPair in = {0};
	in.Source = GetStdHandle(STD_INPUT_HANDLE);
	in.Destination = PipeIn;
	in.Linux = true;
	in.Name.assign(TEXT("w7e-in"));
	in.Thread= CreateThread( NULL, 0, Redirector, &in, 0, NULL);

	TRedirectorPair out = {0};
	out.Destination = GetStdHandle(STD_OUTPUT_HANDLE);
	out.Source = PipeOut;
	out.Name.assign(TEXT("w7e-out"));
	out.Thread= CreateThread( NULL, 0, Redirector, &out, 0, NULL);

	TRedirectorPair err = {0};
	err.Destination = GetStdHandle(STD_ERROR_HANDLE);
	err.Source = PipeErr;
	err.Name.assign(TEXT("w7e-err"));
	err.Thread= CreateThread( NULL, 0, Redirector, &err, 0, NULL);

	HANDLE waiters[3] = { in.Thread, out.Thread, err.Thread };
	WaitForMultipleObjects( 3, waiters, FALSE, INFINITE );

	return 0;
}

bool IsDefaultProcess ( std::pair<DWORD, std::wstring> pair ) {
	return lstrcmpi( pair.second.c_str(), TEXT("explorer.exe") ) == 0;
}

//
//	To avoid some problems with deadlocked processes we need to find way how to run program 
//	once more. Since program uses named papes, it can not be started twice (in current realization).
//	So, if instance of this process already exists, we need to kill it. Regular exe, started from the
//	user's account has no access to kill existing app.
//	Here i use named event to listen for and perform suicide. So, i just need to set this event (if one)
//	and already existsing app will kill itself.
//
DWORD WINAPI Suicide( LPVOID Parameter ) 
{
	CLogger::LogLine(TEXT("Waiting for suicide..."));
	WaitForSingleObject( reinterpret_cast<HANDLE>( Parameter ), INFINITE );
	SetEvent( reinterpret_cast<HANDLE>( Parameter ) );
	CLogger::LogLine(TEXT("Suicide..."));
	ExitProcess( EXIT_FAILURE );
	
	return EXIT_SUCCESS;
}

int _tmain(int argc, _TCHAR* argv[])
{
	CLogger::Reset();
	CLogger::LogLine(TEXT("Started"));

	//
	//	Looking for suicide.
	//
	HANDLE obj = CreateEvent( NULL, FALSE, TRUE, TEXT("ws7Suicide") );
	if ( !obj )
	{
		CLogger::LogLine(TEXT("Unable to create suicide object"));
		ExitProcess( EXIT_FAILURE );
	}

	//
	//	If we see that suicide event is in reset state, we just pulce one and wait for 
	//	it's owner to die. When its done, we acuire this event object and also starting listening for
	//	any signals of this object.
	//
	do
	{
		DWORD rv = WaitForSingleObject( obj, 100 );
		if ( rv == WAIT_OBJECT_0 ) break;

		if ( rv != WAIT_TIMEOUT )
		{
			CLogger::LogLine(TEXT("Suicide wait error"));
			ExitProcess( EXIT_FAILURE );
		}

		CLogger::LogLine(TEXT("Somebody alive. Pulse."));
		PulseEvent( obj );
		Sleep(1000); // wee need to wait;

	}while( true );

	HANDLE hSuicide = CreateThread( NULL, 0, Suicide, obj, 0, NULL );
	if ( !hSuicide )
	{
		CLogger::LogLine(TEXT("Immortals are not allowed"));
		return EXIT_FAILURE;
	}

	do
	{
		int pass_through_index = 1;
		if ( argc <= pass_through_index )
		{
			std::cout << "Too few arguments" << std::endl;
			break;
		}


		DWORD pid = 0;
		if ( lstrcmpi( argv[1], TEXT("--pid") ) == 0 )
		{
			pass_through_index = 3;
			if ( argc <= pass_through_index ) 
			{
				std::cout << "Too few arguments" << std::endl;
				break;
			}

			std::wistringstream pid_stream( argv[2] );
			if ( ! ( pid_stream >> pid ) )
			{
				std::cout << "Invalid pid" << std::endl;
				pid = 0;
			}
		}

		if ( ! pid )
		{
			std::map< DWORD, std::wstring > procs;
			if (!W7EUtils::GetProcessList(GetConsoleWindow(), procs))
			{
				std::cout << "Unable to obtain list of processes" << std::endl;
				break;
			}

			std::map< DWORD, std::wstring >::const_iterator iter = std::find_if( procs.begin(), procs.end(), IsDefaultProcess );
			if (iter == procs.end())
			{
				std::cout << "Unable to find default process" << std::endl;
				break;
			}

			pid = (*iter).first;
		}

		TOKEN_ELEVATION_TYPE g_tet = TokenElevationTypeDefault;
		if (!W7EUtils::GetElevationType(&g_tet))
		{
			_tprintf(_T("GetElevationType failed"));
			break;
		}

		switch(g_tet)
		{
		default:
		case TokenElevationTypeDefault:
			CLogger::LogLine(_T("<< UNKNOWN elevation level. >>\n"));
			break;
		case TokenElevationTypeFull:
			CLogger::LogLine(_T("*** Since the program is already elevated the tests below are fairly meaningless. Re-run it without elevation. ***\n"));
			break;
		case TokenElevationTypeLimited:
			CLogger::LogLine(_T("This program attempts to bypass Windows 7's default UAC settings to run the specified command with silent elevation.\n"));
			break;
		}

		W7EUtils::CTempResource dllResource(NULL, IDD_EMBEDDED_DLL);
		std::wstring strOurDllPath;
		if (!dllResource.GetFilePath(strOurDllPath))
		{
			//MessageBox(GetConsoleWindow(), L"Error extracting dll resource.", L"W7Elevate", MB_OK | MB_ICONERROR);
			CLogger::LogLine(TEXT("Error extracting dll resource."));
			break;
		}

		//
		//	Extraction TIOR.exe from resources and saves exe in the folder where current application 
		//	exists.
		//
		W7EUtils::CTempResource TIORResource(NULL, IDD_EMBEDDED_TIOR);
		std::wstring strOurTIORPath;
		std::wstring tior;
		bool tior_succeed = false;
		if (TIORResource.GetFilePath(strOurTIORPath))
		{
			CLogger::LogLine(TEXT("TIOR extracted"));

			TCHAR me_buff[MAX_PATH];
			DWORD me_count = GetModuleFileName( NULL, me_buff, MAX_PATH );
			if ( me_count )
			{
				TCHAR *me_tail = me_buff + me_count - 1;
				for( ; me_tail > me_buff; me_tail-- )
					if ( *me_tail == '\\' )
					{
						me_tail++;
						*me_tail = 0;
						break;
					}

				tior.assign(me_buff);
				tior.append( TEXT("tior.exe") );

				if ( CopyFile( strOurTIORPath.c_str(), tior.c_str(), FALSE ) )
				{
					CLogger::LogLine(TEXT("TIOR copied"));
					tior_succeed = true;
				}
			}
		}

		if ( tior_succeed )
		{
			tior_succeed = false;

			CInterprocessStorage *tior_storage = CInterprocessStorage::Create( TEXT("w7e_TIORPath") );
			if ( tior_storage )
			{
				CLogger::LogLine(TEXT("TIOR path set"));
				tior_storage->SetString( tior );
				tior_succeed = true;
			}
		}

		if ( !tior_succeed )
		{
			//MessageBox(GetConsoleWindow(), L"Error extracting tior resource.", L"W7Elevate", MB_OK | MB_ICONERROR);
			CLogger::LogLine(L"Error extracting tior resource.");
			break;
		}

		std::wstring args;
		for ( int i = pass_through_index; i < argc; i++ )
		{
			bool q = wcsstr(argv[i], TEXT(" ")) || wcsstr(argv[i], TEXT("\t"));

			if ( q ) args.append( TEXT("\"") );
			args.append( argv[i] );
			if ( q ) args.append( TEXT("\"") );
			args.append( TEXT(" ") );
		}

		if ( !SetupNamedPipe() )
			std::cout << "Unable to setup named pipe" << std::endl;

		//
		//	Preparing shared variables to be used by TIOR that is going to start after we will inject
		//	and load dll into elevated process.
		//
		CInterprocessStorage::Create( TEXT("w7e_TIORShell"), std::wstring(TEXT("cmd.exe")) );
		CInterprocessStorage::Create( TEXT("w7e_TIORArgs"), args );
		CInterprocessStorage::Create( TEXT("w7e_TIORDir"), std::wstring(TEXT("C:\\Windows\\System32")) );

		W7EInject::AttemptOperation(
			GetConsoleWindow(), 
			true, 
			true, 
			pid, 
			TEXT("n/a"), 
			argv[pass_through_index], 
			args.c_str(), 
			TEXT("C:\\Windows\\System32"), 
			strOurDllPath.c_str(), 
			Redirector);

		return EXIT_SUCCESS;

	}while(false);

	PrintUsage();
	return EXIT_FAILURE;
}
