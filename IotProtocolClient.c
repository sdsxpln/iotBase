/*
 * IotProtocol.c
 *
 *  Created on: 11 мар. 2018 г.
 *      Author: blabu
 */
#include "IotProtocolClient.h"
#include "frame.h"
#include "transport.h"
#include "crypt.h"
#include "MyString.h"

#include "logging.h"
/*
 * Result message has a form $V1xxxxYYYY=AAA...AAAcc
 * where '$' - start symbol
 * V1 - version of protocol (0 - F posible variant)
 * xxxx - MESSAGE_SIZE message size ascii format (max 'FFFF')
 * YYYY - unique device identificator
 *  '=' - delim symbol between identifier and arguments
 *  AAA...AAA - arguments of function (may be binary) and can be crypted
 *  cc - CRC16 binary не зашифрованный
 *  Для шифрования используется симметричный алгоритм AES128 (с ключом в 16 байт)
 *  С каждой сессией передачи ключ меняется
 *  Заголовок пакета должен быть в формате ASCII строк для обеспечения совместивмости с предыдущими версиями
 * */
#define KEY_SIZE 16

static const string_t OK = "OK;";
static const string_t ASK = "?;";
static const string_t keyAttribute = "KEY;";

static bool_t isSecure = FALSE;
static u16 DeviceId = 0; // Id устройства
static u08 CryptKey[KEY_SIZE]; // Ключ шифрования
ProtocolStatus_t currentStatus;

static u08 BufTransmit[PROTOCOL_BUFFER_SIZE];
#define BufReceive BufTransmit /*The same buffer*/
//u08 BufReceive[PROTOCOL_BUFFER_SIZE];

void SetId(u16 id){
	if(id) DeviceId = id;
}

void Initialize() {
	getParameters(&DeviceId, CryptKey, KEY_SIZE);
	//isSecure = TRUE;
	execCallBack(Initialize);
}

void EnableSecurity(bool_t on_off){
	isSecure = on_off;
}

ProtocolStatus_t GetLastStatus() {
	return currentStatus;
}

static bool_t isCorrect(u16 id);

// Отправка сообщения обновления ключа шифрования
void WriteClient(u16 size, byte_ptr message) {
	static u08 count = 0;
	static byte_ptr cypherMsg = NULL;
	u16 sz;
	if(!DeviceId) {
		currentStatus = DEVICEID_IS_NULL;
		count = 0;
		execCallBack(WriteClient);
		return;
	}
	if(currentStatus && currentStatus != STATUS_OK) count = 0xFF;
	writeLogU32(count);
	switch(count) {
	case 0:
		if(cypherMsg != NULL) freeMem(cypherMsg);
		sz = size;
		while((sz & 0x0F) & 0x0F) sz++; // Дополняем размер до кратного 16-ти байт (размер блока)
		if(sz > PROTOCOL_BUFFER_SIZE) {count = 0xFF; currentStatus = STATUS_NO_SEND; break;}
		cypherMsg = allocMem(sz);
		if(cypherMsg == NULL) {
			count=0xFF;
			currentStatus = MEMMORY_ALOC_ERR;
			break;
		}
		count++;
		break;
	case 1: // Шифруем и отправляем
		sz = getAllocateMemmorySize(cypherMsg);
		byte_ptr tempMessage = message;
		if(sz > size) {
			tempMessage = allocMem(sz); if(tempMessage == NULL) {currentStatus = STATUS_NO_SEND; break;}
			memCpy(tempMessage,message,size);
			memSet(tempMessage+size,sz-size,0); // Дополняем до кратного 16-ти исходное сообщение нулями
		}
		for(u08 i = 0; i<sz; i+=KEY_SIZE) {
			if(isSecure) AesEcbEncrypt(tempMessage+i,CryptKey,cypherMsg+i);
			else memCpy(cypherMsg+i,tempMessage+i,KEY_SIZE);
		}
		freeMem(tempMessage);
		sz = formFrame(PROTOCOL_BUFFER_SIZE, BufTransmit, DeviceId, sz, cypherMsg);
		if(!sz) {
			count=0xFF;
			currentStatus = STATUS_NO_SEND;
			break;
		}
		count++;
		registerCallBack((TaskMng)WriteClient, size, (BaseParam_t)message, sendTo);
		SetTask((TaskMng)sendTo, sz, BufTransmit);
		return;
	case 2: // Получаем ответ (новый ключ шифрования если все впорядке)
		count++;
		memSet(BufReceive,PROTOCOL_BUFFER_SIZE,0); // Очищаем буфер
		registerCallBack((TaskMng)WriteClient, size, (BaseParam_t)message, receiveFrom);
		SetTask((TaskMng)receiveFrom, PROTOCOL_BUFFER_SIZE, BufReceive); // Ждем ответ
		return;
	case 3: // Парсим ответ
		sz = getAllocateMemmorySize(cypherMsg);
		u16 tempId = parseFrame(PROTOCOL_BUFFER_SIZE, BufReceive, sz ,cypherMsg);
		if(tempId != DeviceId) {
			if(isCorrect(DeviceId)) { // Если устройство имеет корректный идентификационный номер
				currentStatus = STATUS_NO_SEND; // То мы получили не свой пакет
				count = 0xFF;
				break;
			}else if(tempId) { // Если наше устройство имеет не корректный номер, то вероятно это была регистрация
				DeviceId = tempId;
			}
		}
		if(!tempId) {
			currentStatus = STATUS_NO_SEND; // То мы получили пустышку
			count = 0xFF;
			break;
		}
		for(u08 i = 0; i<sz; i+=KEY_SIZE) {
			if(isSecure) AesEcbDecrypt(cypherMsg+i,CryptKey,BufReceive+i); // Расшифровуем полученное сообщение
			else memCpy(BufReceive+i, cypherMsg+i,KEY_SIZE); // Без шифрования
		}
		// Анализ данных
		s16 poz = findStr(keyAttribute,cypherMsg);
		if(poz < 0) {
			currentStatus = STATUS_BAD_KEY;
			count = 0xFF;
			break;
		}
		count++;
		memCpy(CryptKey,BufReceive+poz+strSize(keyAttribute),KEY_SIZE);
		registerCallBack((TaskMng)WriteClient,size,(BaseParam_t)message, saveParameters);
		saveParameters(DeviceId,CryptKey,KEY_SIZE);
		currentStatus = STATUS_OK;
		return;
	case 4: // Отправка подтверждения о получении ключа шифрования
		sz = formFrame(PROTOCOL_BUFFER_SIZE, BufTransmit, DeviceId, strSize(OK), OK);
		count++;
		registerCallBack((TaskMng)WriteClient,size,(BaseParam_t)message, sendTo);
		SetTask(sendTo,sz, BufTransmit);
		return;
	case 5:
	default:
		count = 0;
		freeMem(cypherMsg); cypherMsg = NULL;
		execCallBack(WriteClient);
		return;
	}
	SetTask((TaskMng)WriteClient,size,message);
}

// Чтение данных с сервера
// Отправляет запрос на сервер ?;Максимальный размер читаемых данных. В ответ получим данные
void ReadClient(u16 size, byte_ptr result) {
	static u08 count = 0;
	u08 temp[KEY_SIZE], cypherMsg[KEY_SIZE];
	u16 tempId;
	byte_ptr temp_ptr;
	u08 sz;
	writeLogU32(count);
	if(currentStatus && currentStatus != STATUS_OK) count = 0xFF;
	switch(count){
	case 0:
		if(size > PROTOCOL_BUFFER_SIZE) {// Больше этого мы считать не сможем
			count = 0xFF;
			currentStatus = STATUS_NO_RECEIVE;
			break;
		}
		temp[0] = 0; strCat(temp,ASK);
		toString(2,size,temp+strSize(temp));
		sz = strSize(temp);
		memSet(temp+sz,KEY_SIZE-sz,0); // Дополняем нулями отсавшееся пространство
		if(isSecure) AesEcbEncrypt(temp,CryptKey,cypherMsg); // Эта информация точно будет меньше 16-ти байт (одного блока)
		else memCpy(cypherMsg,temp,sz); // Если без шифрования
		sz = formFrame(PROTOCOL_BUFFER_SIZE, BufTransmit, DeviceId, KEY_SIZE, cypherMsg);
		if(sz) { // Если формирование фрейма прошло удачно
			count++;
			registerCallBack((TaskMng)ReadClient,size,result,sendTo);
			SetTask((TaskMng)sendTo,sz,BufTransmit);
			return;
		}
		currentStatus = STATUS_NO_RECEIVE;
		count = 0xFF;
		break;
	case 1:  // Собственно само чтение
		count++;
		registerCallBack((TaskMng)ReadClient,size,result,receiveFrom);
		memSet(BufReceive,PROTOCOL_BUFFER_SIZE,0); // Очищаем буфер
		receiveFrom(PROTOCOL_BUFFER_SIZE,BufReceive);
		return;
	case 2: // Разшифровуем данные
		sz = size;
		while((sz & 0x0F) & 0x0F) sz++; // Дополняем размер до кратного 16-ти байт (размер блока)
		temp_ptr = allocMem(sz);
		if(temp_ptr == NULL) {
			count = 0xFF;
			currentStatus = MEMMORY_ALOC_ERR;
			break;
		}
		tempId = parseFrame(PROTOCOL_BUFFER_SIZE, BufReceive, sz, temp_ptr);
		if(tempId != DeviceId) {
			freeMem(temp_ptr);
			count = 0xFF;
			currentStatus = STATUS_NO_RECEIVE;
			break;
		}
		for(u08 i = 0; i<sz; i+=KEY_SIZE) {
			if(isSecure) AesEcbDecrypt(temp_ptr+i,CryptKey,BufReceive+i);
			else memCpy(BufReceive+i,temp_ptr+i,KEY_SIZE);
		}
		memCpy(result,BufReceive,size);
		freeMem(temp_ptr);
		count++;
		break;
	case 3:
		sz = formFrame(PROTOCOL_BUFFER_SIZE, BufTransmit, DeviceId, strSize(OK), OK);
		count++;
		registerCallBack((TaskMng)ReadClient,size,result,sendTo);
		sendTo(sz,BufTransmit);
		currentStatus = STATUS_OK;
		return;
	case 4:
	default:
		count = 0;
		execCallBack(ReadClient);
		return;
	}
	SetTask((TaskMng)ReadClient,size,result);
}

static bool_t isCorrect(u16 id) {
	if(id > 0xFF) return TRUE;
	return FALSE;
}


//static u16 generateNewId(u08 type) {
//	u08 temp = RandomSimple() & 0xFF;
//	return ((u16)type<<8) & temp;
//}
//
//static void generateKey(byte_ptr key) {
//	for(u08 i = 0; i<KEY_SIZE; i+=4) {
//		u32 temp = RandomSimple();
//		*((u32*)(key+i)) = temp;
//	}
//}
//
//static void ClientWork(BaseSize_t count, BaseParam_t buffer) {
//	u08 tempContent[PROTOCOL_BUFFER_SIZE];
//	switch(count){
//	case 0:{
//		u16 TempId = parseFrame(getAllocateMemmorySize(buffer),buffer,PROTOCOL_BUFFER_SIZE,tempContent);
//		if(!TempId) {memSet(buffer,getAllocateMemmorySize(buffer),0); count--; break;}
//		if(TempId < 0xFF) { // Если ID < 255 значит это тип
//			s16 poz = findStr(registerAttribute,tempContent);
//			if(poz < 0) {memSet(buffer,getAllocateMemmorySize(buffer),0); count--; break;}
//			TempId = generateNewId((u08)TempId);
//			memSet(tempContent,KEY_SIZE,0);
//			generateKey(tempContent);
//			u16 sz = formFrame(getAllocateMemmorySize(buffer), buffer, TempId, KEY_SIZE, tempContent);
//			count++;
//			registerCallBack(ClientWork, count, buffer, sendTo);
//			SetTask(sendTo,sz,buffer);
//			return;
//		} else {
//
//		}
//	}
//	case 1: // wait OK
//		count++;
//		registerCallBack(ClientWork,count,buffer,receiveFrom);
//		receiveFrom(getAllocateMemmorySize(buffer),buffer);
//	case 2:// check OK
//	default:
//		execCallBack(ClientWork);
//		return;
//	}
//}
//
//void ServerWork(BaseSize_t count, BaseParam_t buffer) {
//	switch(count) {
//	case 0:
//		if(buffer != NULL) freeMem(buffer);
//		buffer = allocMem(PROTOCOL_BUFFER_SIZE);
//		if(buffer == NULL) {count = 0xFF; break;}
//		memSet(buffer,getAllocateMemmorySize(buffer),0);
//		count++;
//		//no break;
//	case 1:
//		count++;
//		registerCallBack(ServerWork,count,buffer,receiveFrom);
//		SetTask(receiveFrom,getAllocateMemmorySize(buffer),buffer);
//		return;
//	case 2:
//		if(!str1_str2(header,(string_t)buffer)) { count--; break; } // Заголовок должен присутствовать
//		registerCallBack(ServerWork,0,buffer,ClientWork);
//		SetTask(ClientWork,0,buffer);
//		return;
//	default:
//		freeMem(buffer);
//		execCallBack(ServerWork);
//		return;
//	}
//	SetTask(ServerWork,count,buffer);
//}


