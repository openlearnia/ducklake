import { expect, test } from '@playwright/test';

test('executes the materialized-view scenario in the custom browser runtime', async ({ page }) => {
  await page.goto('/');
  await expect(page.getByText('Engine ready')).toBeVisible({ timeout: 120_000 });
  await page.getByRole('button', { name: /Run SQL/ }).click();

  await expect(page.getByText('3 rows')).toBeVisible({ timeout: 120_000 });
  await expect(page.locator('tbody')).toContainText('North');
  await expect(page.locator('tbody')).toContainText('440');
});

test('executes the first-class JavaScript procedure scenario', async ({ page }) => {
  await page.goto('/');
  await expect(page.getByText('Engine ready')).toBeVisible({ timeout: 120_000 });
  await page.getByRole('button', { name: 'JavaScript procedures' }).click();
  await page.getByRole('button', { name: /Run SQL/ }).click();

  await expect(page.getByText('1 row')).toBeVisible({ timeout: 120_000 });
  await expect(page.locator('tbody')).toContainText('120');
});
