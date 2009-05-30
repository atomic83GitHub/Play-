#include <string.h>
#include <assert.h>
#include <boost/lexical_cast.hpp>
#include "Dmac_Channel.h"
#include "DMAC.h"
#include "RegisterStateFile.h"

#define STATE_PREFIX            ("dmac/channel_")
#define STATE_SUFFIX            (".xml")
#define STATE_REGS_CHCR         ("CHCR")
#define STATE_REGS_MADR         ("MADR")
#define STATE_REGS_QWC          ("QWC")
#define STATE_REGS_TADR         ("TADR")
#define STATE_REGS_SCCTRL       ("SCCTRL")
#define STATE_REGS_ASR0         ("ASR0")
#define STATE_REGS_ASR1         ("ASR1")

using namespace std;
using namespace Dmac;
using namespace boost;

CChannel::CChannel(CDMAC& dmac, unsigned int nNumber, const DmaReceiveHandler& pReceive) :
m_dmac(dmac),
m_nNumber(nNumber),
m_pReceive(pReceive)
{

}

CChannel::~CChannel()
{

}

void CChannel::Reset()
{
	memset(&m_CHCR, 0, sizeof(CHCR));
	m_nMADR		= 0;
	m_nQWC		= 0;
	m_nTADR		= 0;
	m_nSCCTRL	= 0;
}

void CChannel::SaveState(CZipArchiveWriter& archive)
{
    string path = STATE_PREFIX + lexical_cast<string>(m_nNumber) + STATE_SUFFIX;
    CRegisterStateFile* registerFile = new CRegisterStateFile(path.c_str());
    registerFile->SetRegister32(STATE_REGS_CHCR,    m_CHCR);
    registerFile->SetRegister32(STATE_REGS_MADR,    m_nMADR);
    registerFile->SetRegister32(STATE_REGS_QWC,     m_nQWC);
    registerFile->SetRegister32(STATE_REGS_TADR,    m_nTADR);
    registerFile->SetRegister32(STATE_REGS_SCCTRL,  m_nSCCTRL);
    registerFile->SetRegister32(STATE_REGS_ASR0,    m_nASR[0]);
    registerFile->SetRegister32(STATE_REGS_ASR1,    m_nASR[1]);
    archive.InsertFile(registerFile);
}

void CChannel::LoadState(CZipArchiveReader& archive)
{
    string path = STATE_PREFIX + lexical_cast<string>(m_nNumber) + STATE_SUFFIX;
    CRegisterStateFile registerFile(*archive.BeginReadFile(path.c_str()));
    m_CHCR      <<= registerFile.GetRegister32(STATE_REGS_CHCR);
    m_nMADR       = registerFile.GetRegister32(STATE_REGS_MADR);
    m_nQWC        = registerFile.GetRegister32(STATE_REGS_QWC);
    m_nTADR       = registerFile.GetRegister32(STATE_REGS_TADR);
    m_nSCCTRL     = registerFile.GetRegister32(STATE_REGS_SCCTRL);
    m_nASR[0]     = registerFile.GetRegister32(STATE_REGS_ASR0);
    m_nASR[1]     = registerFile.GetRegister32(STATE_REGS_ASR1);
}

uint32 CChannel::ReadCHCR()
{
	return *(uint32*)&m_CHCR;
}

void CChannel::WriteCHCR(uint32 nValue)
{
	bool nSuspend = false;

	//We need to check if the purpose of this write is to suspend the current transfer
    if(m_dmac.m_D_ENABLE & CDMAC::ENABLE_CPND)
	{
        nSuspend = (m_CHCR.nSTR != 0) && ((nValue & CDMAC::CHCR_STR) == 0);
	}

	if(m_CHCR.nSTR == 1)
	{
		m_CHCR.nSTR = ~m_CHCR.nSTR;
        m_CHCR.nSTR = ((nValue & CDMAC::CHCR_STR) != 0) ? 1 : 0;
	}
	else
	{
		m_CHCR = *(CHCR*)&nValue;
	}

    if(m_CHCR.nSTR != 0)
    {
        m_nSCCTRL |= SCCTRL_INITXFER;
        Execute();
    }
}

void CChannel::Execute()
{
	if(m_CHCR.nSTR != 0)
	{
        if(m_dmac.m_D_ENABLE)
        {
            if(m_nNumber != 4)
            {
                throw runtime_error("Need to check that case.");
            }
            return;
        }
        switch(m_CHCR.nMOD)
		{
		case 0x00:
			ExecuteNormal();
			break;
		case 0x01:
			ExecuteSourceChain();
			break;
		default:
			assert(0);
			break;
		}
	}
}

void CChannel::ExecuteNormal()
{
	uint32 nRecv = m_pReceive(m_nMADR, m_nQWC, 0, false);

	m_nMADR	+= nRecv * 0x10;
	m_nQWC	-= nRecv;

	if(m_nQWC == 0)
	{
		ClearSTR();
	}
}

void CChannel::ExecuteSourceChain()
{
	uint64 nTag;
	uint32 nRecv;
	uint8 nID;

	//Execute current
	if(m_nQWC != 0)
	{
		nRecv = m_pReceive(m_nMADR, m_nQWC, 0, false);

		m_nMADR	+= nRecv * 0x10;
		m_nQWC	-= nRecv;

		if(m_nQWC != 0)
		{
			//Transfer isn't finished, suspend for now
			return;
		}
	}

	while(m_CHCR.nSTR == 1)
	{
		//Check if we've finished our DMA transfer
		if(m_nQWC == 0)
		{
			if(m_nSCCTRL & SCCTRL_INITXFER)
			{
				//Clear this bit
				m_nSCCTRL &= ~SCCTRL_INITXFER;
			}
			else
			{
                if(CDMAC::IsEndTagId((uint32)m_CHCR.nTAG << 16))
				{
					ClearSTR();
					continue;
				}
			}
		}
		else
		{
			//Suspend transfer
			break;
		}

		if(m_CHCR.nTTE == 1)
		{
            if(m_pReceive(m_nTADR, 1, 0, true) != 1)
            {
                //Device didn't receive DmaTag, break for now
                break;
            }
		}

        //Half-Life does this...
        if(m_nTADR == 0)
        {
            ClearSTR();
            continue;
        }

		nTag = m_dmac.FetchDMATag(m_nTADR);

		//Save higher 16 bits of tag into CHCR
		m_CHCR.nTAG = nTag >> 16;
		//m_nCHCR &= ~0xFFFF0000;
		//m_nCHCR |= nTag & 0xFFFF0000;

		nID = (uint8)((nTag >> 28) & 0x07);

		switch(nID)
		{
		case 0:
			//REFE - Data to transfer is pointer in memory address, transfer is done
			m_nMADR		= (uint32)((nTag >>  32) & 0xFFFFFFFF);
			m_nQWC		= (uint32)((nTag >>   0) & 0x0000FFFF);
			m_nTADR		= m_nTADR + 0x10;
			break;
		case 1:
			//CNT - Data to transfer is after the tag, next tag is after the data
			m_nMADR		= m_nTADR + 0x10;
			m_nQWC		= (uint32)(nTag & 0xFFFF);
			m_nTADR		= (m_nQWC * 0x10) + m_nMADR;
			break;
		case 2:
			//NEXT - Transfers data after tag, next tag is at position in ADDR field
			m_nMADR		= m_nTADR + 0x10;
			m_nQWC		= (uint32)((nTag >>   0) & 0x0000FFFF);
			m_nTADR		= (uint32)((nTag >>  32) & 0xFFFFFFFF);
			break;
		case 3:
			//REF - Data to transfer is pointed in memory address, next tag is after this tag
			m_nMADR		= (uint32)((nTag >>  32) & 0xFFFFFFFF);
			m_nQWC		= (uint32)((nTag >>   0) & 0x0000FFFF);
			m_nTADR		= m_nTADR + 0x10;
			break;
		case 5:
			//CALL - Transfers QWC after the tag, saves next address in ASR, TADR = ADDR
			assert(m_CHCR.nASP < 2);
			m_nMADR				= m_nTADR + 0x10;
			m_nQWC				= (uint32)(nTag & 0xFFFF);
			m_nASR[m_CHCR.nASP]	= m_nMADR + (m_nQWC * 0x10);
			m_nTADR				= (uint32)((nTag >>  32) & 0xFFFFFFFF);
			m_CHCR.nASP++;
			break;
		case 6:
			//RET - Transfers QWC after the tag, pops TADR from ASR
			assert(m_CHCR.nASP > 0);
			m_CHCR.nASP--;

			m_nMADR		= m_nTADR + 0x10;
			m_nQWC		= (uint32)(nTag & 0xFFFF);
			m_nTADR		= m_nASR[m_CHCR.nASP];
			break;
		case 7:
			//END - Data to transfer is after the tag, transfer is finished
			m_nMADR		= m_nTADR + 0x10;
			m_nQWC		= (uint32)(nTag & 0xFFFF);
			break;
		default:
			m_nQWC = 0;
			assert(0);
			break;
		}

		if(m_nQWC != 0)
		{
			nRecv = m_pReceive(m_nMADR, m_nQWC, 0, false);

			m_nMADR		+= nRecv * 0x10;
			m_nQWC		-= nRecv;
		}
	}
}

void CChannel::SetReceiveHandler(const DmaReceiveHandler& handler)
{
    m_pReceive = handler;
}

void CChannel::ClearSTR()
{
	m_CHCR.nSTR = ~m_CHCR.nSTR;

	//Set interrupt
	m_dmac.m_D_STAT |= (1 << m_nNumber);

	m_dmac.UpdateCpCond();
}
