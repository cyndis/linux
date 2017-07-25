/*
 * Copyright (c) 2017 NVIDIA Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#define HOST1X_HV_SYNCPT_PROT_EN			0x1ac4
#define HOST1X_HV_SYNCPT_PROT_EN_CH_EN			BIT(1)
#define HOST1X_HV_CH_KERNEL_FILTER_GBUFFER(x)		(0x2020 + (x * 4))
