#include "gpio.h"

void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef gpio = {0};
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();
  /* Set levels before enabling outputs: CS/RST high, DC/BL low. */
  HAL_GPIO_WritePin(GPIOB, TFT_CS_Pin | TFT_RST_Pin, GPIO_PIN_SET);
  HAL_GPIO_WritePin(GPIOB, TFT_DC_Pin | TFT_BL_Pin, GPIO_PIN_RESET);
  gpio.Pin = TFT_CS_Pin | TFT_DC_Pin | TFT_RST_Pin | TFT_BL_Pin;
  gpio.Mode = GPIO_MODE_OUTPUT_PP;
  gpio.Pull = GPIO_NOPULL;
  gpio.Speed = GPIO_SPEED_FREQ_HIGH;
  HAL_GPIO_Init(GPIOB, &gpio);
}
