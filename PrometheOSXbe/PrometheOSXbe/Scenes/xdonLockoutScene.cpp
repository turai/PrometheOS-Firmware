#include "xdonLockoutScene.h"
#include "..\component.h"
#include "..\drawing.h"
#include "..\theme.h"
#include "..\utils.h"
#include "..\xdonServer.h"

xdonLockoutScene::xdonLockoutScene()
{
	utils::setLedStates(
		SMC_LED_STATES_GREEN_STATE0 | SMC_LED_STATES_RED_STATE0 |
		SMC_LED_STATES_GREEN_STATE1 | SMC_LED_STATES_RED_STATE1 |
		SMC_LED_STATES_RED_STATE2 |
		SMC_LED_STATES_RED_STATE3	
	);
}

xdonLockoutScene::~xdonLockoutScene()
{
	utils::setLedStates(
		SMC_LED_STATES_GREEN_STATE0 |
		SMC_LED_STATES_GREEN_STATE1 |
		SMC_LED_STATES_GREEN_STATE2 |
		SMC_LED_STATES_GREEN_STATE3
	);
}

void xdonLockoutScene::update()
{
}

void xdonLockoutScene::render()
{
	int xPos, yPos;
	char* clients, *bytesFmt, *bytesRead, *bytesWritten;

	component::panel(theme::getPanelFillColor(), theme::getPanelStrokeColor(), 16, 16, 688, 448);
#ifndef TOOLS
	drawing::drawBitmapStringAligned(context::getBitmapFontLarge(), "\xC2\xA7\xC2\xA8\xC2\xA9\xC2\xAA\xC2\xAB\xC2\xAC\xC2\xAD\xC2\xAB\xC2\xA9\xC2\xAE", theme::getPrometheosColor(), theme::getPrometheosAlign(), 40, theme::getPrometheosY(), 640);
#else
	drawing::drawBitmapStringAligned(context::getBitmapFontLarge(), "\xC2\xA7\xC2\xA8\xC2\xA9\xC2\xAA\xC2\xAB\xC2\xAC\xC2\xAD\xC2\xAB\xC2\xA9\xC2\xAE \xC2\xAC\xC2\xA9\xC2\xA9\xC2\xB4\xC2\xAE", theme::getPrometheosColor(), theme::getPrometheosAlign(), 40, theme::getPrometheosY(), 640);
#endif

	yPos = (context::getBufferHeight() - (40 + 44)) / 2;
	yPos += theme::getCenterOffset();

	drawing::drawBitmapStringAligned(context::getBitmapFontMedium(), "XDON is connected", theme::getTextColor(), horizAlignmentCenter, 40, yPos, 640);
	yPos += 40;
	drawing::drawBitmapStringAligned(context::getBitmapFontMedium(), "Don't turn off your console", theme::getTextColor(), horizAlignmentCenter, 40, yPos, 640);

	#define COL_W 190
	xPos = 60;
	clients = stringUtility::formatString("Clients: %d", xdonServer::connectedClients());
	drawing::drawBitmapStringAligned(context::getBitmapFontSmall(), clients, theme::getTextColor(), horizAlignmentLeft, xPos, theme::getFooterY(), COL_W);
	free(clients);

	xPos += COL_W;
	bytesFmt = stringUtility::formatSize(xdonServer::bytesRead());
	bytesRead = stringUtility::formatString("Read: %s", bytesFmt);
	drawing::drawBitmapStringAligned(context::getBitmapFontSmall(), bytesRead, theme::getTextColor(), horizAlignmentCenter, xPos, theme::getFooterY(), COL_W);
	free(bytesRead);
	free(bytesFmt);

	xPos += COL_W;
	bytesFmt = stringUtility::formatSize(xdonServer::bytesWritten());
	bytesWritten = stringUtility::formatString("Written: %s", bytesFmt);
	drawing::drawBitmapStringAligned(context::getBitmapFontSmall(), bytesWritten, theme::getTextColor(), horizAlignmentRight, xPos, theme::getFooterY(), COL_W);
	free(bytesWritten);
	free(bytesFmt);
}