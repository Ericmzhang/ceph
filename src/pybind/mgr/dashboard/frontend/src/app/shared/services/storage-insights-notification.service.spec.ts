import { TestBed } from '@angular/core/testing';

import { HttpClientTestingModule } from '@angular/common/http/testing';
import { RouterTestingModule } from '@angular/router/testing';
import { StorageInsightsNotificationService } from './storage-insights-notification.service';
import { configureTestBed } from '~/testing/unit-test-helper';
import { SharedModule } from '~/app/shared/shared.module';

describe('StorageInsightsNotificationService', () => {
  let service: StorageInsightsNotificationService;

  configureTestBed({
    providers: [StorageInsightsNotificationService],
    imports: [HttpClientTestingModule, RouterTestingModule, SharedModule]
  });

  beforeEach(() => {
    TestBed.configureTestingModule({});
    service = TestBed.inject(StorageInsightsNotificationService);
  });

  it('should be created', () => {
    expect(service).toBeTruthy();
  });
});
