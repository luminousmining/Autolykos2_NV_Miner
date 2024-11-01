// request.cc

/*******************************************************************************

    REQUEST -- Http requests handling

*******************************************************************************/

#include "../include/conversion.h"
#include "../include/definitions.h"
#include "../include/easylogging++.h"
#include "../include/jsmn.h"
#include "../include/processing.h"
#include "../include/request.h"
#include <ctype.h>
#include <curl/curl.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <atomic>
#include <mutex>



uint32_t calcN(uint32_t Hblock)
{
	uint32_t headerHeight;
	((uint8_t *)&headerHeight)[0] = ((uint8_t *)&Hblock)[3];
	((uint8_t *)&headerHeight)[1] = ((uint8_t *)&Hblock)[2];
	((uint8_t *)&headerHeight)[2] = ((uint8_t *)&Hblock)[1];
	((uint8_t *)&headerHeight)[3] = ((uint8_t *)&Hblock)[0];

	uint32_t newN = INIT_N_LEN;
	if (headerHeight < IncreaseStart)
		newN = INIT_N_LEN;
	else if (headerHeight >= IncreaseEnd)
		newN = MAX_N_LEN;
	else
	{
		uint32_t itersNumber = (headerHeight - IncreaseStart) / IncreasePeriodForN + 1;
		for (uint32_t i = 0; i < itersNumber; i++)
		{
			newN = newN / 100 * 105;
		}
	}
	return newN;
}

////////////////////////////////////////////////////////////////////////////////
//  Write function for CURL http GET
////////////////////////////////////////////////////////////////////////////////
size_t WriteFunc(
    void * ptr,
    size_t size,
    size_t nmemb,
    json_t * request
)
{
    size_t newlen = request->len + size * nmemb;

    if (newlen > request->cap)
    {
        request->cap = (newlen << 1) + 1;

        if (request->cap > MAX_JSON_CAPACITY)
        {
            LOG(ERROR) << "Request capacity exceeds json capacity in WriteFunc";
        }

        if (!(request->ptr = (char *)realloc(request->ptr, request->cap)))
        {
            LOG(ERROR) << "Request pointer realloc failed in WriteFunc";
        } 
    }

    memcpy(request->ptr + request->len, ptr, size * nmemb);

    request->ptr[newlen] = '\0';
    request->len = newlen;

    return size * nmemb;
}

////////////////////////////////////////////////////////////////////////////////
//  Lowercase letters convert to uppercase
////////////////////////////////////////////////////////////////////////////////
int ToUppercase(char * str)
{
    for (int i = 0; str[i] != '\0'; ++i) { str[i] = toupper(str[i]); }

    return EXIT_SUCCESS;
}

////////////////////////////////////////////////////////////////////////////////
//  CURL log error 
////////////////////////////////////////////////////////////////////////////////
void CurlLogError(CURLcode curl_status)
{
    if (curl_status != CURLE_OK)
    {
        LOG(ERROR) << "CURL: " << curl_easy_strerror(curl_status);
    }

    return;
}

////////////////////////////////////////////////////////////////////////////////
// Parse JSON request and substitute data if needed
// moved to separate function for tests
///////////////////////////////////////////////////////////////////////////////
int ParseRequest(json_t * oldreq, json_t * newreq, info_t *info, int checkPubKey, long http_code)
{
	jsmn_parser parser;
	int mesChanged = 0;
    int HChanged = 0;
	int boundChanged = 0;
	int ExtraBaseChanged = 0;
	int ExtraSizeChanged = 0;
	ToUppercase(newreq->ptr);
	jsmn_init(&parser);

	
	int numtoks = jsmn_parse(
		&parser, newreq->ptr, newreq->len, newreq->toks, REQ_LEN
		);

	if (numtoks < 0)
	{
		LOG(ERROR) << "Jsmn failed to parse latest block";
		LOG(ERROR) << "Block data: " << newreq->ptr;

		return EXIT_FAILURE;
	}

	int PkPos = -1;
	int BoundPos = -1;
	int MesPos = -1;
    int HPos = -1;
	int ExtraBasePos = -1;
	int ExtraSizePos = -1;

	for (int i = 1; i < numtoks; i += 2)
	{
		if (newreq->jsoneq(i, "B"))
		{
			BoundPos = i + 1;
		}
		else if (newreq->jsoneq(i, "PK"))
		{
			PkPos = i + 1;
		}
		else if (newreq->jsoneq(i, "MSG"))
		{
			MesPos = i + 1;
		}
		else if (newreq->jsoneq(i, "H") || newreq->jsoneq(i, "HEIGHT")  )
		{
			HPos = i + 1;
		}
		else if (newreq->jsoneq(i, "EXTRANONCE1"))
		{
			ExtraBasePos = i + 1;
		}
		else if (newreq->jsoneq(i, "EXTRANONCE2SIZE"))
		{
			ExtraSizePos = i + 1;
		}

		else
		{
			VLOG(1) << "Unexpected field in /block/candidate json";
		}

	}

	(HPos == -1) ? info->AlgVer = 1 : info->AlgVer = 2;
	if ( BoundPos < 0 || MesPos < 0 || HPos < 0 )
	{
		LOG(ERROR) << "Block data: " << newreq->ptr;
		if (BoundPos < 0 && MesPos < 0 && HPos < 0 && http_code == 200)
		{
			LOG(ERROR) << "(http_code 200), problem in  proxy connection";
			info->doJob = false;

		}
		else
		{
			LOG(ERROR) << "Some of expected fields not present in /block/candidate";
		}
		return EXIT_FAILURE;
	}
	info->doJob = true;

	if (checkPubKey)
	{
		if (newreq->GetTokenLen(PkPos) != PK_SIZE_4)
		{
			LOG(ERROR) << "Wrong size pubkey in block info";
			return EXIT_FAILURE;
		}
		if (strncmp(info->pkstr, newreq->GetTokenStart(PkPos), PK_SIZE_4))
		{
			char logstr[1000];

			LOG(ERROR)
				<< "Generated and received public keys do not match";

			PrintPublicKey(info->pkstr, logstr);
			LOG(ERROR) << "Generated public key:\n   " << logstr;

			PrintPublicKey(newreq->GetTokenStart(PkPos), logstr);
			LOG(ERROR) << "Received public key:\n   " << logstr;

			exit(EXIT_FAILURE);
		}
	}

	int mesLen = newreq->GetTokenLen(MesPos);
	int boundLen = newreq->GetTokenLen(BoundPos);
	int Hlen = newreq->GetTokenLen(HPos);
	int ExtraBaseLen = newreq->GetTokenLen(ExtraBasePos);
	int ExtraSizeLen = newreq->GetTokenLen(ExtraSizePos);

	if (oldreq->len)
	{
		if (mesLen != oldreq->GetTokenLen(MesPos)) { mesChanged = 1; }
		else
		{
			mesChanged = strncmp(
				oldreq->GetTokenStart(MesPos),
				newreq->GetTokenStart(MesPos),
				mesLen
				);
		}

		if (boundLen != oldreq->GetTokenLen(BoundPos))
		{
			boundChanged = 1;
		}
		else
		{
			boundChanged = strncmp(
				oldreq->GetTokenStart(BoundPos),
				newreq->GetTokenStart(BoundPos),
				boundLen
				);
		}


		if (ExtraBasePos != -1)
		{
			if (ExtraBaseLen != oldreq->GetTokenLen(ExtraBasePos))
			{
				ExtraBaseChanged = 1;
			}
			else
			{
				ExtraBaseChanged = strncmp(
					oldreq->GetTokenStart(ExtraBasePos),
					newreq->GetTokenStart(ExtraBasePos),
					ExtraBaseLen
					);
			}
		}

		if (ExtraSizePos != -1)
		{
			if (ExtraSizeLen != oldreq->GetTokenLen(ExtraSizePos))
			{
				ExtraSizeChanged = 1;
			}
			else
			{
				ExtraSizeChanged = strncmp(
					oldreq->GetTokenStart(ExtraSizePos),
					newreq->GetTokenStart(ExtraSizePos),
					ExtraSizeLen
					);
			}
		}


        HChanged = strncmp(
            oldreq->GetTokenStart(HPos),
            newreq->GetTokenStart(HPos),
            Hlen
            );

	}

	// check if we need to change anything, only then lock info mutex
	if (mesChanged || boundChanged || !(oldreq->len) || HChanged || ExtraBaseChanged || ExtraSizeChanged)
	{
		info->info_mutex.lock();
		// --------------------check strutum-------------------------------------
		info->stratumMode = 1;
		if (ExtraBasePos == -1)
		{
			memset(info->extraNonceStart, 0, NONCE_SIZE_8);
			memset(info->extraNonceEnd, 1, NONCE_SIZE_8);
			info->stratumMode = 0;
		}
		else if (!(oldreq->len) || ExtraBaseChanged || ExtraSizeChanged)
		{
			if(ExtraSizeLen <= 0)
			{
				LOG(ERROR) << "stratumMode, Invalid nonce \n ";
				LOG(ERROR) << "Block data: " << newreq->ptr;
				info->info_mutex.unlock();
				return EXIT_FAILURE;
			}

			char *buff = new char[ExtraSizeLen];
			memcpy(buff, newreq->GetTokenStart(ExtraSizePos), ExtraSizeLen);
			char *endptr;
			unsigned int iLen = strtoul(buff, &endptr, 10);
			delete buff;

			//iLen = 1;
			iLen *= 2; //hex
			if (info->stratumMode == 1 && (iLen + ExtraBaseLen) != NONCE_SIZE_4)
			{
				LOG(ERROR) << "stratumMode, Invalid nonce \n ";
				LOG(ERROR) << "Block data: " << newreq->ptr;
				info->info_mutex.unlock();
				return EXIT_FAILURE;
			}
			memset(info->extraNonceStart, 0, NONCE_SIZE_8);
			memset(info->extraNonceEnd, 1, NONCE_SIZE_8);

			char *NonceBase = new char[ExtraBaseLen];
			memcpy(NonceBase, newreq->GetTokenStart(ExtraBasePos), ExtraBaseLen);

			char *StartNonce = new char[NONCE_SIZE_4];
			memset(StartNonce, '0', NONCE_SIZE_4);
			char *EndNonce = new char[NONCE_SIZE_4];
			memset(EndNonce, '0', NONCE_SIZE_4);

			memcpy(StartNonce, NonceBase, ExtraBaseLen);

			memcpy(EndNonce, NonceBase, ExtraBaseLen);
			memset(EndNonce + ExtraBaseLen, 'F', iLen);

			HexStrToLittleEndian(
				StartNonce, NONCE_SIZE_4,
				info->extraNonceStart, NONCE_SIZE_8
				);
			HexStrToLittleEndian(
				EndNonce, NONCE_SIZE_4,
				info->extraNonceEnd, NONCE_SIZE_8
				);
			delete NonceBase;
			delete StartNonce;
			delete EndNonce;

		}
		// --------------------check strutum-------------------------------------

		//================================================================//
		//  Substitute message and change state when message changed
		//================================================================//
		if (!(oldreq->len) || mesChanged)
		{
			HexStrToBigEndian(
				newreq->GetTokenStart(MesPos), newreq->GetTokenLen(MesPos),
				info->mes, NUM_SIZE_8
				);
		}
		if (!(oldreq->len)  || HChanged )
		{
			char *buff = new char[Hlen];
			memcpy(buff, newreq->GetTokenStart(HPos), Hlen);
			char *endptr;
			unsigned int ul = strtoul(buff, &endptr, 10);
			info->Hblock[0] = ((uint8_t *)&ul)[3];
			info->Hblock[1] = ((uint8_t *)&ul)[2];
			info->Hblock[2] = ((uint8_t *)&ul)[1];
			info->Hblock[3] = ((uint8_t *)&ul)[0];
			delete buff;
		}

		//================================================================//
		//  Substitute bound in case it changed
		//================================================================//
		if (!(oldreq->len) || boundChanged)
		{
			char buf[NUM_SIZE_4 + 1];

			DecStrToHexStrOf64(
				newreq->GetTokenStart(BoundPos),
				newreq->GetTokenLen(BoundPos),
				buf
				);

			HexStrToLittleEndian(buf, NUM_SIZE_4, info->bound, NUM_SIZE_8);
		}





		info->info_mutex.unlock();

		// signaling uint
		++(info->blockId);
		LOG(INFO) << "Got new block in main thread, block data: " << newreq->ptr;
	}

	return EXIT_SUCCESS;


}



////////////////////////////////////////////////////////////////////////////////
//  CURL http GET request
////////////////////////////////////////////////////////////////////////////////
int GetLatestBlock(
    const char * from,
    json_t * oldreq,
    info_t * info,
    int checkPubKey
)
{
    CURL * curl;
    json_t newreq(0, REQ_LEN);

    //========================================================================//
    //  Get latest block
    //========================================================================//
    CURLcode curlError;

    curl = curl_easy_init();
    if (!curl) { LOG(ERROR) << "CURL initialization failed in GetLatestBlock"; }

    CurlLogError(curl_easy_setopt(curl, CURLOPT_URL, from));
    CurlLogError(curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteFunc));
    CurlLogError(curl_easy_setopt(curl, CURLOPT_WRITEDATA, &newreq));
    
    // set timeout to 30 sec so it doesn't hang up
    // waiting for default 5 minutes if url is unreachable / wrong 
    CurlLogError(curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L));
    CurlLogError(curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L));
    curlError = curl_easy_perform(curl);
   	long http_code = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    CurlLogError(curlError);
    curl_easy_cleanup(curl);

    VLOG(1) << "GET request " << newreq.ptr;
    
    // if curl returns error on request, do not change or check anything 
    if (!curlError)
    {
        unsigned int oldId = info->blockId.load();
        if(ParseRequest(oldreq, &newreq, info, checkPubKey,http_code) != EXIT_SUCCESS)
        {
            return EXIT_FAILURE;
        }
        //====================================================================//
        //  Substitute old block with newly read
        //====================================================================//
        if(oldId != info->blockId.load())
        {
            FREE(oldreq->ptr);
            FREE(oldreq->toks);
            *oldreq = newreq;
            newreq.ptr = NULL;
            newreq.toks = NULL;
        }

        return EXIT_SUCCESS;
    }
	
    info->doJob = false;

    return EXIT_FAILURE;
}


////////////////////////////////////////////////////////////////////////////////
//  CURL http notification, Completed job 
////////////////////////////////////////////////////////////////////////////////

int JobCompleted(
	const char * to
	)
{
	CURL * curl;
	json_t newreq(0, REQ_LEN);

	//========================================================================//
	//  JOB COMPLETED
	//========================================================================//
	CURLcode curlError;

	curl = curl_easy_init();
	if (!curl) { LOG(ERROR) << "CURL initialization failed in JobCompleted"; }

	CurlLogError(curl_easy_setopt(curl, CURLOPT_URL, to));
	CurlLogError(curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteFunc));
	CurlLogError(curl_easy_setopt(curl, CURLOPT_WRITEDATA, &newreq));

	// set timeout to 30 sec so it doesn't hang up
	// waiting for default 5 minutes if url is unreachable / wrong
	CurlLogError(curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L));
	CurlLogError(curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L));
	curlError = curl_easy_perform(curl);
	CurlLogError(curlError);
	curl_easy_cleanup(curl);

	LOG(INFO) << "GET request (JOB Completed) " << newreq.ptr;

	// if curl returns error on request, do not change or check anything
	if (!curlError)
	{
	}

	return EXIT_SUCCESS;

}

////////////////////////////////////////////////////////////////////////////////
//  CURL http POST request
////////////////////////////////////////////////////////////////////////////////
int PostPuzzleSolution(
    const char * to,
    const uint8_t * nonce
)
{
    uint32_t pos = 0;

    char request[JSON_CAPACITY];


    //========================================================================//
    //  Form message to post
    //========================================================================//



    strcpy(request + pos, "{\"n\":\"");
    pos += 6;

    LittleEndianToHexStr(nonce, NONCE_SIZE_8, request + pos);
    pos += NONCE_SIZE_4;

    strcpy(request + pos, "\"}\0");



    VLOG(1) << "POST request " << request;
    LOG(INFO) << "POST request " << request;


    //========================================================================//
    //  POST request
    //========================================================================//
    CURL * curl;
    curl = curl_easy_init();

    if (!curl)
    {
        LOG(ERROR) << "CURL initialization failed in PostPuzzleSolution";
    }

    json_t respond(0, REQ_LEN);
    curl_slist * headers = NULL;
    curl_slist * tmp;
    CURLcode curlError;
    tmp = curl_slist_append(headers, "Accept: application/json");
    headers = curl_slist_append(tmp, "Content-Type: application/json");

    CurlLogError(curl_easy_setopt(curl, CURLOPT_URL, to));
    CurlLogError(curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers));;
    CurlLogError(curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request));
    
    // set timeout to 30 sec for sending solution
    CurlLogError(curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L));
    CurlLogError(curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L));    
    CurlLogError(curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteFunc));
    CurlLogError(curl_easy_setopt(curl, CURLOPT_WRITEDATA, &respond));

    int retries = 0;

    do
    {
        curlError = curl_easy_perform(curl);
        ++retries;
    }
    while (retries < MAX_POST_RETRIES && curlError != CURLE_OK);

    
    
    CurlLogError(curlError);

    LOG(INFO) << "Node response:" << respond.ptr;

    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);

    return EXIT_SUCCESS;
}

// request.cc


