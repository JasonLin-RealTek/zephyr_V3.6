/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Copyright (c) 2025 Realtek Semiconductor Corporation, SIBG-SD7
 *
 */

#define DT_DRV_COMPAT realtek_rts5912_sha

#include <errno.h>
#include <string.h>
#include <zephyr/init.h>
#include <zephyr/crypto/crypto.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(sha256_hw_rtk, CONFIG_CRYPTO_LOG_LEVEL);

BUILD_ASSERT(DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT) == 1,
	     "only one realtek,rts5912-sha compatible node can be supported");

#include "reg/reg_crypto.h"
#include "crypto_rts5912_priv.h"

static struct rts5912_sha256_context rts5912_sha256_ctx;

#define RTS5912_SHA2DMA_MAXIMUM_BLOCK_NUM (0x1FFUL)
#define RTS5912_SHA2DMA_BLOCK_SIZE        (64)
#define RTS5912_SHA2DMA_BLOCK_SHIFT       (6)
#define RTS5912_SHA2DMA_8Byte_SHIFT       (3)
#define RTS5912_SHA2DMA_DST_WIDTH         (0x3)
#define RTS5912_SHA2DMA_SRC_WIDTH         (0x3)
#define RTS5912_SHA2DMA_HIGH_LEVEL_MSK    (0xFFFF0000)
#define RTS5912_SHA2_BLOCK_EXTEND_CHECK   (56)
#define RTS5912_MAXIMUM_CRYPTO_POLLING_TIMES (50)

static void rts5912_sha256_start(const struct device *dev)
{
	const struct rts5912_sha_config *cfg = dev->config;
	volatile struct sha2_type *sha2_regs = (volatile struct sha2_type *) cfg->cfg_sha2_regs;
	volatile struct sha2dma_type *sha2dma_regs = (volatile struct sha2dma_type *) cfg->cfg_sha2dma_regs;

	if (rts5912_sha256_ctx.is224) {
		sha2_regs->ctrl = SHA2_CTRL_BYTEINV_Msk | SHA2_CTRL_ICGEN_Msk;
		for (uint32_t i = 0; i < 8; i++) {
			sha2_regs->digest[i << 1] = rts5912_sha224_digest[i];
			sha2_regs->digest[(i << 1) + 1] = 0x0;
		}
	} else {
		sha2_regs->ctrl = SHA2_CTRL_RST_Msk | SHA2_CTRL_BYTEINV_Msk | SHA2_CTRL_ICGEN_Msk;
	}

	sha2dma_regs->dma_enable = SHA2DMA_DMA_ENABLE_Msk;
	sha2dma_regs->config = 0x0;
	sha2dma_regs->dar = 0x0;
	sha2dma_regs->ctrl_low = (sha2dma_regs->ctrl_low & RTS5912_SHA2DMA_HIGH_LEVEL_MSK) |
				 (SHA2DMA_CTRL_INT_EN_Msk |
				  (RTS5912_SHA2DMA_DST_WIDTH << SHA2DMA_CTRL_DST_WIDTH_Pos) |
				  (RTS5912_SHA2DMA_SRC_WIDTH << SHA2DMA_CTRL_SRC_WIDTH_Pos) |
				  (0x2 << SHA2DMA_CTRL_SRC_BURST_Pos));
	sha2dma_regs->msk_transfer = (sha2dma_regs->msk_transfer & RTS5912_SHA2DMA_HIGH_LEVEL_MSK) |
				     SHA2DMA_MSKTFR_INT_EN_Msk | SHA2DMA_MSKTFR_INT_WRITE_EN_Msk;
	sha2dma_regs->msk_block = 0x0;
}

static int rts5912_sha256_process(const struct device *dev, uint8_t *input, size_t blk_size)
{
	const struct rts5912_sha_config *cfg = dev->config;
	volatile struct sha2_type *sha2_regs = (volatile struct sha2_type *) cfg->cfg_sha2_regs;
	volatile struct sha2dma_type *sha2dma_regs = (volatile struct sha2dma_type *) cfg->cfg_sha2dma_regs;
	uint32_t idx = 0, sha2_waiting_times = 0;

	for (; blk_size > 0; blk_size -= RTS5912_SHA2DMA_MAXIMUM_BLOCK_NUM) {
		sha2dma_regs->sar = (uint32_t)(&(input[idx]));
		if (blk_size <= RTS5912_SHA2DMA_MAXIMUM_BLOCK_NUM) {
			sha2dma_regs->ctrl_high = blk_size << RTS5912_SHA2DMA_8Byte_SHIFT;
		} else {
			sha2dma_regs->ctrl_high = RTS5912_SHA2DMA_MAXIMUM_BLOCK_NUM
						  << RTS5912_SHA2DMA_8Byte_SHIFT;
		}
		sha2dma_regs->channel_enable = SHA2DMA_CHEN_EN_Msk | SHA2DMA_CHEN_WRITE_EN_Msk;
		while (!(sha2dma_regs->interrupt_status &
			 (SHA2DMA_INTSTS_TFR_COMPLETE_Msk | SHA2DMA_INTSTS_BLK_COMPLETE_Msk |
			  SHA2DMA_INTSTS_SCR_COMPLETE_Msk | SHA2DMA_INTSTS_DST_COMPLETE_Msk |
			  SHA2DMA_INTSTS_BUS_COMPLETE_Msk))) {
			if(sha2_waiting_times < RTS5912_MAXIMUM_CRYPTO_POLLING_TIMES){
				sha2_waiting_times++;
				k_busy_wait(USEC_PER_MSEC);
			}else{
				LOG_ERR("SHA2DMA reach timeout and breach");
				return -EIO;
			}
		}

		if (sha2dma_regs->interrupt_status & SHA2DMA_INTSTS_BUS_COMPLETE_Msk) {
			return -EIO;
		}

		sha2dma_regs->clear_transfer = SHA2DMA_INTCLR_CLRTFR_Msk;
		idx += RTS5912_SHA2DMA_MAXIMUM_BLOCK_NUM << RTS5912_SHA2DMA_BLOCK_SHIFT;

		if (blk_size <= RTS5912_SHA2DMA_MAXIMUM_BLOCK_NUM) {
			break;
		}
	}

	for (uint32_t i = 0; i < 8; i++) {
		rts5912_sha256_ctx.state[i] = sha2_regs->digest[i << 1];
	}

	return 0;
}

static int rts5912_sha256_update(const struct device *dev, uint8_t *input, size_t len)
{
	uint32_t remain, fill, blk_size = 0, ret_val = 0;

	remain = rts5912_sha256_ctx.total[0] & (RTS5912_SHA2DMA_BLOCK_SIZE - 1);
	fill = RTS5912_SHA2DMA_BLOCK_SIZE - remain;

	rts5912_sha256_ctx.total[0] += len;
	if (rts5912_sha256_ctx.total[0] < len) {
		rts5912_sha256_ctx.total[1]++;
	}

	if ((len >= fill) && (remain != 0)) {
		memcpy((void *)(&(rts5912_sha256_ctx.buffer[remain])), input, fill);
		ret_val = rts5912_sha256_process(dev, (&(rts5912_sha256_ctx.buffer[0])), 0x1);
		if (ret_val != 0) {
			return ret_val;
		}

		input += fill;
		len -= fill;
		remain = 0;
	}

	while (len >= RTS5912_SHA2DMA_BLOCK_SIZE) {
		blk_size = len >> RTS5912_SHA2DMA_BLOCK_SHIFT;
		ret_val = rts5912_sha256_process(dev, input, blk_size);
		if (ret_val != 0) {
			return ret_val;
		}
		input += (len & ~(RTS5912_SHA2DMA_BLOCK_SIZE - 1));
		len &= (RTS5912_SHA2DMA_BLOCK_SIZE - 1);
	}

	if (len > 0) {
		memcpy((void *)(&(rts5912_sha256_ctx.buffer[remain])), input, len);
	}

	return ret_val;
}

static int rts5912_sha256_finish(const struct device *dev, uint8_t *output)
{
	uint32_t remain, be_high, be_low, ret_val = 0;

	remain = rts5912_sha256_ctx.total[0] & (RTS5912_SHA2DMA_BLOCK_SIZE - 1);
	rts5912_sha256_ctx.buffer[remain++] = 0x80U;

	if (remain <= RTS5912_SHA2_BLOCK_EXTEND_CHECK) {
		memset((void *)(&(rts5912_sha256_ctx.buffer[remain])), 0x0,
		       RTS5912_SHA2_BLOCK_EXTEND_CHECK - remain);
	} else {
		memset((void *)(&(rts5912_sha256_ctx.buffer[remain])), 0x0,
		       RTS5912_SHA2DMA_BLOCK_SIZE - remain);
		ret_val = rts5912_sha256_process(dev, (uint8_t *)(&(rts5912_sha256_ctx.buffer[0])),
						 0x1);
		if (ret_val != 0) {
			return ret_val;
		}
		memset((void *)(&(rts5912_sha256_ctx.buffer[0])), 0x0,
		       RTS5912_SHA2_BLOCK_EXTEND_CHECK);
	}

	be_high = (rts5912_sha256_ctx.total[0] >> (32 - RTS5912_SHA2DMA_8Byte_SHIFT)) |
		  (rts5912_sha256_ctx.total[1] << RTS5912_SHA2DMA_8Byte_SHIFT);
	be_low = rts5912_sha256_ctx.total[0] << RTS5912_SHA2DMA_8Byte_SHIFT;
	*(uint32_t *)(&(rts5912_sha256_ctx.buffer[56])) = sys_cpu_to_be32(be_high);
	*(uint32_t *)(&(rts5912_sha256_ctx.buffer[60])) = sys_cpu_to_be32(be_low);

	ret_val = rts5912_sha256_process(dev, (uint8_t *)(&(rts5912_sha256_ctx.buffer[0])), 0x1);
	if (ret_val != 0) {
		return ret_val;
	}

	volatile uint32_t *output_buf = (volatile uint32_t *)output;

	for (uint32_t i = 0; i < 7; i++) {
		output_buf[i] = sys_cpu_to_be32(rts5912_sha256_ctx.state[i]);
	}
	if (rts5912_sha256_ctx.is224 == 0) {
		output_buf[7] = sys_cpu_to_be32(rts5912_sha256_ctx.state[7]);
	}

	return ret_val;
}

static int rts5912_sha256_handler(struct hash_ctx *ctx, struct hash_pkt *pkt, bool finish)
{
	memcpy(rts5912_sha256_ctx.sha2_data_in_sram, pkt->in_buf, pkt->in_len);

	if (finish) {
		int ret_val = rts5912_sha256_update(
			ctx->device, rts5912_sha256_ctx.sha2_data_in_sram, pkt->in_len);
		if (ret_val) {
			return ret_val;
		}
		return rts5912_sha256_finish(ctx->device, pkt->out_buf);
	} else {
		return rts5912_sha256_update(ctx->device, rts5912_sha256_ctx.sha2_data_in_sram,
					     pkt->in_len);
	}
}

static int rts5912_hash_begin_session(const struct device *dev, struct hash_ctx *ctx,
				      enum hash_algo algo)
{
	switch (algo) {
	case CRYPTO_HASH_ALGO_SHA224:
	case CRYPTO_HASH_ALGO_SHA256:
		memset((void *)(&(rts5912_sha256_ctx)), 0x0, sizeof(struct rts5912_sha256_context));
		rts5912_sha256_ctx.is224 = (algo == CRYPTO_HASH_ALGO_SHA224);
		if (ctx) {
			ctx->hash_hndlr = rts5912_sha256_handler;
		}
		rts5912_sha256_start(dev);
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int rts5912_hash_free_session(const struct device *dev, struct hash_ctx *ctx)
{
	return 0;
}

static inline int rts5912_query_hw_caps(const struct device *dev)
{
	return (CAP_SEPARATE_IO_BUFS | CAP_SYNC_OPS);
}

static int rts5912_sha_init(const struct device *dev)
{
	uint8_t init_buf[32] = {0};
	struct hash_pkt pkt = {
		.in_buf = init_buf,
		.out_buf = init_buf,
		.in_len = 32,
		.ctx = NULL,
	};

	rts5912_hash_begin_session(dev, pkt.ctx, CRYPTO_HASH_ALGO_SHA256);
	rts5912_sha256_update(dev, pkt.in_buf, pkt.in_len);
	rts5912_sha256_finish(dev, pkt.out_buf);

	return 0;
}

static DEVICE_API(crypto, rts5912_hash_funcs) = {
	.hash_begin_session = rts5912_hash_begin_session,
	.hash_free_session = rts5912_hash_free_session,
	.query_hw_caps = rts5912_query_hw_caps,
};

const struct rts5912_sha_config cfg_0 = {
	.cfg_sha2_regs = (volatile struct sha2_type *) DT_INST_REG_ADDR_BY_NAME(0, sha2),
	.cfg_sha2dma_regs = (volatile struct sha2dma_type *) DT_INST_REG_ADDR_BY_NAME(0, sha2dma),
};

DEVICE_DT_INST_DEFINE(0, &rts5912_sha_init, NULL, NULL, &cfg_0, POST_KERNEL,
		      CONFIG_CRYPTO_INIT_PRIORITY, (void *)&rts5912_hash_funcs);
